// Build 15 seconds OHLC Bar chart data from trade data and publish the same thru websockets to clients
// Author: Yuvaraja Subramaniam ( www.linkedin.com/in/yuvaraja )

/*
	Thread 1: Reads the trade data from the json file
	Thread 2: FSM that calculates the 15 seconds OHLC bar from trade data
	Thread 3: Maintains client subscriptions and sends bar info to clients
*/

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <cstdint>
#include <pthread.h>
#include <poll.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>

//includes for Seasocks websocket library
#include "seasocks/PrintfLogger.h"
#include "seasocks/Server.h"
#include "seasocks/StringUtil.h"
#include "seasocks/WebSocket.h"
#include "seasocks/util/Json.h"
#include <cstring>
#include <memory>
#include <set>

using namespace std;
using namespace seasocks;

//Trade Packet (Sent from Worker 1 to Worker 2)
struct tradepacket {
	char   sym[15];
	double price;
	double qty;
	uint64_t ts2;
};

//15 Seconds Bar Context
struct BarCntxt {
	char         sym[15];
	unsigned int bar_num;
	uint64_t     bar_start_time;
	uint64_t     bar_close_time;
	double		 bar_open;
	double       bar_high;
	double       bar_low;
	double       bar_close;
	double       bar_volume;
};

//Bar Types
enum Bar_Type {  CLOSING_BAR = 0, 
                 TRADE_BAR = 1, 
                 TIMER_EXP_CLOSING_BAR = 2, 
                 TIMER_EXP_OPENING_BAR = 3, 
                 BAR_TYPE_COUNT
              };

vector<string> Bar_Type_Name = { "CLOSING_BAR", 
                                 "TRADE_BAR", 
                                 "TIMER_EXP_CLOSING_BAR", 
                                 "TIMER_EXP_OPENING_BAR", 
                                 "BAR_TYPE_INVALID" 
                               };

//FSM States
enum FSM_States {  FSM_STARTING = 0, 
                   FSM_READY = 1, 
                   FSM_DOWN = 2, 
                   FSM_STATE_COUNT 
                };

vector<string> FSM_State_Name = { "FSM_STARTING", 
                                  "FSM_READY", 
                                  "FSM_DOWN", 
                                  "FSM_STATE_INVALID" 
                                };



//Events processed by the FSM and the associated data
enum FSM_Event_Types { TRADE_PKT_ARRIVAL = 0, 
                       TIMER_EXPIRY = 1, 
                       EVENT_TYPE_COUNT
                     };

vector<string> FSM_Event_Type_Name = { "TRADE_PKT_ARRIVAL", 
                                       "TIMER_EXPIRY", 
                                       "EVENT_TYPE_INVALID" 
                                     };


//FSM Events Data

struct FSM_Event_Data_Trade_Pkt {
	char   sym[15];
	double price;
	double qty;
	uint64_t ts2;
};


struct FSM_Event_Data_Timer_Exp {
	uint64_t ts;
};


union FSM_Event_Data {
	FSM_Event_Data_Trade_Pkt trd_pkt;
	FSM_Event_Data_Timer_Exp tmr_exp;
};

struct FSM_EVENT {
	FSM_Event_Types type;
	FSM_Event_Data  data;
};


//FSM Event Handler Type
typedef bool (*FSM_EVENT_HANDLER) (FSM_EVENT &);


//Function prototypes
void *trade_data_reader(void *msg);
void *fsm_thread_bar_calc(void *msg);
void *publisher_thread_publish_bars(void *msg);
bool fsm_fire_event(FSM_EVENT & fsm_ev);
bool process_fsm_starting(FSM_EVENT & fsm_ev);
bool process_fsm_down(FSM_EVENT & fsm_ev);
bool process_fsm_ready_ev_trd_pkt_arrival(FSM_EVENT & fsm_ev);
bool process_fsm_ready_ev_tmr_expiry(FSM_EVENT & fsm_ev);
bool process_fsm_event(FSM_EVENT & fsm_ev);
bool fsm_emit_bar(BarCntxt barcntxt, Bar_Type bt);

vector<string> tokenize(const char *str, char c);
bool parse_trade(string line, map<string, string> & trdmap);

FSM_EVENT_HANDLER FSM_Ev_Handler_Table[FSM_STATE_COUNT][EVENT_TYPE_COUNT] = {
																				process_fsm_starting, process_fsm_starting,
																				process_fsm_ready_ev_trd_pkt_arrival, process_fsm_ready_ev_tmr_expiry,
																				process_fsm_down, process_fsm_down	
																			};


int pfd_w1_w2[2];  
int pfd_w2_w3[2];

FSM_States fsm_curr_state = FSM_STARTING;
map<string, BarCntxt> bar_cntxt_cache;
map<string, BarCntxt> outbound_cache;
map<string, BarCntxt> pubs_bar_cache;

const uint64_t fifteen_sec_nanosecs = 15 * 1000000000UL ;

int main()
{
	const char *tradefile = "trades.json";
	pthread_t trade_reader;
	pthread_t fsm_thread;
	pthread_t publisher_thread;

	//create the pipe for data sharing between worker 1 and worker 2
	pipe(pfd_w1_w2);

	//create the pipe for data sharing between worker 2 and worker 3
	pipe(pfd_w2_w3);


	int retval_1;

	retval_1 = pthread_create(&trade_reader, NULL, trade_data_reader, (void *) tradefile);

	int retval_2;
	const char *fsm = "FSM Thread";
	retval_2 = pthread_create(&fsm_thread, NULL, fsm_thread_bar_calc, (void *) fsm);

	int retval_3;
	const char *publisher = "Publisher Thread";
	retval_3 = pthread_create(&publisher_thread, NULL, publisher_thread_publish_bars, (void *) publisher);

	pthread_join(trade_reader, NULL);
	pthread_join(fsm_thread, NULL);
	pthread_join(publisher_thread, NULL);

	return 0;
}




//Thread 1: Read the trade data, format trade packets and deliver to FSM
void *trade_data_reader(void *msg) {

	char *fname = static_cast<char*>(msg);

	//open the file
	ifstream trdfile(fname);

	string line;
	while(getline(trdfile, line)) {
		//cout << "Read line: " << line << endl;
		string delchars = "{} \"";
		for (char c: delchars) {
			line.erase( remove(line.begin(), line.end(), c), line.end());
		}
		//cout << "Trade Reader Thread => Stripped line: " << line << endl;

		map<string, string> trademap;
		parse_trade(line, trademap);

		string symbol;
		double price;
		double qty;
		uint64_t ts2;

		for (auto itr = trademap.begin(); itr != trademap.end(); itr++) {
			string key = itr->first;
			string val = itr->second;

			//cout << "map key = " << key << ", map value = " << val << endl;
			if (key == "sym") {
				symbol = val;
			}
			else if (key == "P") {
				istringstream is(val);
				is>>price;
			}
			else if (key == "Q") {
				istringstream is(val);
				is>>qty;
			}
			else if (key == "TS2") {
				istringstream is(val);
				is>>ts2;
			}
		}
		
		//cout << "Trade Reader Thread => Parsed Trade: " << "sym = " << symbol << ", P = " << price << ", Q = " << qty << ", TS2 = " << ts2 << endl;

		//Create trade packet and deliver
		tradepacket tp;
		strcpy(tp.sym, symbol.c_str());
		tp.price = price;
		tp.qty   = qty;
		tp.ts2   = ts2;

		//write into the pipe that takes the data to fsm thread
		write(pfd_w1_w2[1], &tp, sizeof(tp));
	}
}


//Parse trade data into a map of values
bool parse_trade(string line, map<string, string> & trdmap) {

	char item_delimiter = ',';
	char key_delimiter  = ':';
	vector<string> items;
	items = tokenize(line.c_str(), item_delimiter);

	for (string item : items) {
		//cout << "item : " << item << endl;

		vector<string> keyval = tokenize(item.c_str(), key_delimiter);
		string ky  = keyval[0];
		string val = keyval[1];
		//cout << "key = " << ky << ", val = " << val << endl;
		//add to the parsemap
		trdmap.insert(pair<string, string>(ky, val));
	}

	return true;
}



//Tokenizing function
vector<string> tokenize(const char *str, char c)
{
    vector<string> result;

    do
    {
        const char *begin = str;

        while(*str != c && *str)
            str++;

        result.push_back(string(begin, str));
    } while (0 != *str++);

    return result;
}



//Thread 2: FSM thread. Reads trade packets from Worker 1 and calculates bar OHLC values
void *fsm_thread_bar_calc(void *msg)
{
	struct pollfd fds[1];
	fds[0].fd = pfd_w1_w2[0];
	fds[0].events = POLLIN;
	tradepacket tp;

	//set fsm_curr_state to FSM_READY
	fsm_curr_state = FSM_READY;

	while(1) {
		int timeout_msecs = 2 * 1000;
		int ret = poll(fds, 1, timeout_msecs);

		if (ret > 0) {
			if (fds[0].revents & POLLIN) {

				if ( fcntl( fds[0].fd, F_SETFL, fcntl(fds[0].fd, F_GETFL) | O_NONBLOCK ) < 0 ) {
					cout << "FSM Thread => Error setting nonblocking flag for incoming data pipe" << endl;
					exit(0);
				}

				int r;
				while( (r = read(fds[0].fd, &tp, sizeof(tp))) > 0)  {
					char symbol[15];
					strcpy(symbol, tp.sym);
					double price = tp.price;
					double qty   = tp.qty;
					uint64_t ts2 = tp.ts2; 

					//cout << "FSM Thread => read tradepacket : sym = " << symbol << ", P = " << price << ", Q = " << qty << ", TS2 = " << ts2 << endl;
					//Create a trade packet arrival event and fire it

					FSM_EVENT fsm_ev;
					fsm_ev.type = TRADE_PKT_ARRIVAL;
					strcpy(fsm_ev.data.trd_pkt.sym, symbol);
					fsm_ev.data.trd_pkt.price = price;
					fsm_ev.data.trd_pkt.qty = qty;
					fsm_ev.data.trd_pkt.ts2 = ts2;
					fsm_fire_event(fsm_ev);
				}
			}
		}
		else {
			cout << "FSM Thread => Timeout occured while reading trade data. No trade data to read from source pipe fd" << endl;
		}
	}
}


//Fire FSM Event
bool fsm_fire_event(FSM_EVENT & fsm_ev) {
	process_fsm_event(fsm_ev);
return true;
}

//Process events while FSM_State == FSM_STARTING. Ignore all events received at this stage
bool process_fsm_starting(FSM_EVENT & fsm_ev) {

	cout << "FSM Thread => event arrived. Ignoring as state = " << FSM_State_Name[fsm_curr_state] << endl;
return true;
}

//Process events while FSM_State == FSM_DOWN. Ignore all events received at this stage
bool process_fsm_down(FSM_EVENT & fsm_ev) {

	cout << "FSM Thread => event arrived. Ignoring as state = " << FSM_State_Name[fsm_curr_state] << endl;
return true;
}


//Process events TRADE_PKT_ARRIVAL while FSM_State == FSM_READY
bool process_fsm_ready_ev_trd_pkt_arrival(FSM_EVENT & fsm_ev) {
	
	char symbol[15];
	strcpy(symbol, fsm_ev.data.trd_pkt.sym);
	double price = fsm_ev.data.trd_pkt.price;
	double qty   = fsm_ev.data.trd_pkt.qty;
	uint64_t ts2 = fsm_ev.data.trd_pkt.ts2; 
	uint64_t expired_timestamp = ts2; 

	//cout << "FSM Thread => event arrived = trd_pkt_arrival: sym = " << symbol << ", P = " << price << ", Q = " << qty << ", TS2 = " << ts2 << endl;

	//check if symbol exists in Bar contexts cache
	string sym(symbol);

	//cout << "FSM Thread => Searching bar cache for symbol " << symbol << endl;

	auto it = bar_cntxt_cache.find(sym);

	bool cntxt_exists = (it != bar_cntxt_cache.end());

	//cout << "FSM Thread => cntxt_exists = " << cntxt_exists << endl;

	if (!cntxt_exists) {
		//bars context does not exist. create it
	    //cout << "FSM Thread => Bar context does not exist. Creating it : sym = " << symbol << endl;
		BarCntxt newcntxt;
		strcpy(newcntxt.sym, symbol);
		newcntxt.bar_num        = 1;
		newcntxt.bar_start_time = ts2;
		newcntxt.bar_close_time = ts2 + fifteen_sec_nanosecs;
		newcntxt.bar_open       = price;
		newcntxt.bar_high       = price;
		newcntxt.bar_low        = price;
		newcntxt.bar_close      = price;
		newcntxt.bar_volume     = qty;
	
		//store the new context in cache
		bar_cntxt_cache.insert( pair<string, BarCntxt>(sym, newcntxt) );

		//TODO - update subscribers on bar open
		fsm_emit_bar(newcntxt, TRADE_BAR);
	}
	else {
		//bars context exist, update it
	    //cout << "FSM Thread => Bar context exists. Update it : sym = " << symbol << endl;
		BarCntxt oldcntxt = it->second;
	    //cout << "FSM Thread => sym = " << symbol << ", Current bar close time = " << oldcntxt.bar_close_time << ", Current TS2 : " << ts2 << endl;
		if ( ts2 <= oldcntxt.bar_close_time) {
	    //cout << "FSM Thread => sym = " << symbol << ". Trade goes into exising bar" << endl;
		//TODO - remove debug
		//fsm_emit_bar(oldcntxt, TRADE_BAR);

			//trade goes into existing bar
			if (price > oldcntxt.bar_high) {
				oldcntxt.bar_high = price;
			}

			if (price < oldcntxt.bar_low ) {
				oldcntxt.bar_low  = price;
			}

			oldcntxt.bar_close    = price;
			oldcntxt.bar_volume  += qty;
			it->second = oldcntxt;
		
		//TODO - remove debug
		//TODO - update subscribers on trade update
		fsm_emit_bar(oldcntxt, TRADE_BAR);
		}
		else {
			    //trade goes into next bar or someother future bar
	    		//cout << "FSM Thread => sym = " << symbol << ". Trade goes into next bar or future bar" << endl;
				uint64_t curr_bar_close_time = oldcntxt.bar_close_time;
				do {
					// keep closing the current bar until the bar that accomodates the current trade opens up
					BarCntxt newcntxt;
					strcpy(newcntxt.sym, symbol);
					newcntxt.bar_num        = oldcntxt.bar_num + 1;
					newcntxt.bar_start_time = oldcntxt.bar_close_time + 1;
					newcntxt.bar_close_time = newcntxt.bar_start_time + fifteen_sec_nanosecs;
					newcntxt.bar_open       = oldcntxt.bar_close;
					newcntxt.bar_high       = oldcntxt.bar_close;
					newcntxt.bar_low        = oldcntxt.bar_close;
					newcntxt.bar_close      = oldcntxt.bar_close;
					newcntxt.bar_volume     = 0;

					//emit closing bar info to worker 3 
					fsm_emit_bar(oldcntxt, CLOSING_BAR);

					//update the bar cache with current bar info
					it->second = newcntxt;
					curr_bar_close_time = newcntxt.bar_close_time;
					oldcntxt = newcntxt;

				} while (ts2 > curr_bar_close_time ); 
				
				//the intermediate bars have been closed. update the current bar with current trade info
				oldcntxt.bar_open     = price;
				oldcntxt.bar_high     = price;
				oldcntxt.bar_low      = price;
				oldcntxt.bar_close    = price;
				oldcntxt.bar_volume  += qty;
				it->second = oldcntxt;

				//TODO - update subscribers on trade update
				fsm_emit_bar(oldcntxt, TRADE_BAR);
		}
	}

	//Create a timer expiry event for the currently processed UTC timestamp. Let's all progress together, bring others along

	//Ideally there should be a timer expiry event triggered by the system every few microseconds. But we don't have time for that
	//when we try to replay the existing trades and build bars for existing trades. May be in future we can make all three threads
	//react to a single underlying timer that emits timer expiry events that becomes the blood flow of the system and triggers
	//time-scynced processing everywhere.

	//This timer-expiry will hook along the processing in other tickers as well
	FSM_EVENT fsm_tmr_ev;
	fsm_tmr_ev.type = TIMER_EXPIRY;
	fsm_tmr_ev.data.tmr_exp.ts = expired_timestamp;
	fsm_fire_event(fsm_tmr_ev);

return true;
}




//Process events TIMER_EXPIRY while FSM_State == FSM_READY
bool process_fsm_ready_ev_tmr_expiry(FSM_EVENT & fsm_ev) {
	uint64_t expired_ts = fsm_ev.data.tmr_exp.ts; 
	//cout << "FSM Thread => event arrived = timer_expiry: " << "TS = " << expired_ts << endl;

	//Iterate the bar cache and close the bars that have expired
	for( auto it = bar_cntxt_cache.begin() ; it != bar_cntxt_cache.end() ; it++ ) {
		string   sym       = it->first;
		BarCntxt barcntxt  = it->second;
		uint64_t bar_close_time = barcntxt.bar_close_time; 
		//cout << "FSM Thread => processing timer_expiry: " << "symbol = " << sym << ", bar_close_time = " << bar_close_time << ", expired_ts = " << expired_ts << endl;

	 while (expired_ts > bar_close_time ) {

			// keep closing the current bar until the bar that accomodates the current expired timestamp opens up
			BarCntxt newcntxt;
			strcpy(newcntxt.sym, barcntxt.sym);
			newcntxt.bar_num        = barcntxt.bar_num + 1;
			newcntxt.bar_start_time = barcntxt.bar_close_time + 1;
			newcntxt.bar_close_time = newcntxt.bar_start_time + fifteen_sec_nanosecs;
			newcntxt.bar_open       = barcntxt.bar_close;
			newcntxt.bar_high       = barcntxt.bar_close;
			newcntxt.bar_low        = barcntxt.bar_close;
			newcntxt.bar_close      = barcntxt.bar_close;
			newcntxt.bar_volume     = 0;

			//emit closing bar info to worker 3 
			fsm_emit_bar(barcntxt, TIMER_EXP_CLOSING_BAR);

			//emit opening bar info to worker 3. (do not emit opening bars, emit bars only on closure of bars or trades)
			//fsm_emit_bar(newcntxt, TIMER_EXP_OPENING_BAR);

			//update the bar cache with current bar info
			it->second = newcntxt;
			bar_close_time = newcntxt.bar_close_time;
			barcntxt = newcntxt;
		}
	} 
return true;
}



//Process FSM Event - Demulitplex based on the fsm_curr_state and event type
bool process_fsm_event(FSM_EVENT & fsm_ev) {
	FSM_Event_Types ev_type = fsm_ev.type;	
	//cout << "FSM Thread => dispatching event " << FSM_Event_Type_Name[ev_type] << " to handler function" << endl; 
	(*FSM_Ev_Handler_Table[fsm_curr_state][ev_type])(fsm_ev);

return true;
}


//Emit bar into to worker 3
bool fsm_emit_bar(BarCntxt barcntxt, Bar_Type bt){

	//closing price is 0.0 for bars that are not closing bars. i.e trade bars / open bars etc
	//actual closing price is emited only for bar types CLOSING_BAR and TIMER_EXP_CLOSING_BAR

	if (bt == TRADE_BAR or bt == TIMER_EXP_OPENING_BAR) {
		barcntxt.bar_close = 0.0;
	}

	char symbol[15];
	unsigned int bar_num   = barcntxt.bar_num;
	double bar_open        = barcntxt.bar_open;
	double bar_high        = barcntxt.bar_high;
	double bar_low         = barcntxt.bar_low;
	double bar_close       = barcntxt.bar_close;
	double bar_volume      = barcntxt.bar_volume;
	uint64_t bar_start_time  = barcntxt.bar_start_time;
	uint64_t bar_close_time  = barcntxt.bar_close_time;
	strcpy(symbol, barcntxt.sym);

	string sym(symbol);

	bool emit_bar = true;

	//check if bar exists in outboud cache. emit the bar only in case of new bars / update of existing bars
	auto it = outbound_cache.find(sym);
	bool bar_exists = (it != outbound_cache.end());
	//cout << "FSM Thread => bar_exists = " << bar_exists << endl;

	if ( bar_exists == true ) {
		BarCntxt prevctxt = it->second;
	
		if ( bar_num        == prevctxt.bar_num        and
		     bar_open       == prevctxt.bar_open       and
		     bar_high       == prevctxt.bar_high       and
		     bar_low        == prevctxt.bar_low        and
		     bar_close      == prevctxt.bar_close      and
		     bar_volume     == prevctxt.bar_volume     and
		     bar_start_time == prevctxt.bar_start_time and
		     bar_close_time == prevctxt.bar_close_time ) {
			//the bar need not be emitted if the values have not changed
			emit_bar = false;
			//cout << "FSM Thread => Ignoring bar. No update in existing bar. " << "bartype = " << Bar_Type_Name[bt] 
            //     << ", symbol = " << symbol 
            //     << ", bar_num = " << bar_num << endl;
		}
	}

	if (emit_bar == true) {
		if (bar_exists == true) {
			//update existing entry in the outbound cache
			it->second = barcntxt;
		} else {
			//insert new entry in the outbound cache
			outbound_cache.insert( pair<string, BarCntxt>(sym, barcntxt) );
		}
		//cout << "FSM Thread => Emiting Bar : " 
		//             << "bartype = "          << Bar_Type_Name[bt] 
		//             << ", symbol = "         << symbol 
		//             << ", bar_num = "        << bar_num
		//             << ", O = "              << bar_open
		//             << ", H = "              << bar_high
		//             << ", L = "              << bar_low
		//             << ", C = "              << bar_close
		//             << ", volume = "         << bar_volume
		//             << ", bar_start_time = " << bar_start_time
		//             << ", bar_close_time = " << bar_close_time
		//             << endl;

		//write bar context into the pipe that takes the data to publisher thread
		write(pfd_w2_w3[1], &barcntxt, sizeof(barcntxt));
	}
}



//Seasocks websockets libray handlers client side service

class MyHandler : public WebSocket::Handler {
public:
    explicit MyHandler(Server* server)
            : _server(server) {
    }

    void onConnect(WebSocket* connection) override {
        _connections.insert(connection);
        std::cout << "Connected: " << connection->getRequestUri()
                  << " : " << formatAddress(connection->getRemoteAddress())
                  << "\nCredentials: " << *(connection->credentials()) << "\n";
    }

    void onData(WebSocket* connection, const char* data) override {
        if (0 == strcmp("die", data)) {
            _server->terminate();
            return;
        }
        if (0 == strcmp("close", data)) {
            std::cout << "Closing..\n";
            connection->close();
            std::cout << "Closed.\n";
            return;
        }

        string ticker = string(data);
		string msg    = ticker + " - Is this what you requested ?";
        connection->send(msg.c_str());
    }

    void onDisconnect(WebSocket* connection) override {
        _connections.erase(connection);
        std::cout << "Disconnected: " << connection->getRequestUri()
                  << " : " << formatAddress(connection->getRemoteAddress()) << "\n";
    }

    void publishBar(BarCntxt barcntxt) {
		for (auto connection : _connections) {
        	std::cout << "Publishing bar to : " << connection->getRequestUri()
                  << " : " << formatAddress(connection->getRemoteAddress()) << "\n";
			stringstream ss;
			ss << "{\"event\": \"ohlc_notify\", ";
			ss << "\"symbol\": \"" << barcntxt.sym       << "\", ";
			ss << "\"bar_num\": "  << barcntxt.bar_num   << ", ";
			ss << "\"O\": "        << barcntxt.bar_open  << ", ";
			ss << "\"H\": "        << barcntxt.bar_high  << ", ";
			ss << "\"L\": "        << barcntxt.bar_low   << ", ";
			ss << "\"C\": "        << barcntxt.bar_close << ", ";
			ss << "\"volume\": "   << barcntxt.bar_volume;
			ss << "}";
        
			connection->send(ss.str());
		}
    }

private:
    std::set<WebSocket*> _connections;
    Server* _server;
};




//Thread 3: Publisher thread. Receive bars from FSM thread and publish to clients.
//Maintain client connections and subscriptions

void *publisher_thread_publish_bars(void *msg)
{
	const  int numfds =2;
	struct pollfd fds[numfds];


	//register pipe for incoming data activity
	fds[0].fd = pfd_w2_w3[0];
	fds[0].events = POLLIN;
	BarCntxt barcntxt;

	cout << "Starting Seasocks server" << endl;

	//Seasocks logger
    auto logger = std::make_shared<PrintfLogger>(Logger::Level::Debug);

    Server server(logger);

    auto handler = std::make_shared<MyHandler>(&server);
    server.addWebSocketHandler("/", handler);
    server.startListening(9090);

	int server_fd = server.fd();
	cout << "Websocks server fd : " << server_fd << endl;

	//Register server fd for any subscription activity
	fds[1].fd = server_fd;
	fds[1].events = POLLIN;

	while (1) {

		int timeout_msecs = 1 * 100;
		int ret = poll(fds, numfds, timeout_msecs);

		if (ret > 0) {

			//first check the websocket server for subscriptions
			if (fds[1].revents & POLLIN) {

    			server.poll(100);
			}
			
			//subscription cache is update now. process the outgoing bars
			if (fds[0].revents & POLLIN) {

				if ( fcntl( fds[0].fd, F_SETFL, fcntl(fds[0].fd, F_GETFL) | O_NONBLOCK ) < 0 ) {
					cout << "Pubisher Thread => Error setting nonblocking flag for incoming bars data pipe" << endl;
					exit(0);
				}

				int r;
				while( (r = read(fds[0].fd, &barcntxt, sizeof(barcntxt))) > 0)  {

					string symbol(barcntxt.sym);
					//update the publishers bar cache
					auto it = pubs_bar_cache.find(symbol);
					bool bar_exists = ( it != pubs_bar_cache.end() );

					if (bar_exists == true) {
						//update existing entry in the publisher cache
						it->second = barcntxt;
					} else {
						//insert new entry in the publisher cache
						pubs_bar_cache.insert( pair<string, BarCntxt>(symbol, barcntxt) );
					}
					
					//TODO - Check the subscriptions and push the bar to subscribers thru appopriate client connection socket descriptors
					//cout << "Publisher Thread => Read incoming bar : "
					//     << "sym = "              << barcntxt.sym 
					//     << ", bar_num = "        << barcntxt.bar_num
					//     << ", bar_start_time = " << barcntxt.bar_start_time
					//     << ", bar_close_time = " << barcntxt.bar_close_time
					//     << ", bar_open = "       << barcntxt.bar_open
					//     << ", bar_high = "       << barcntxt.bar_high
					//     << ", bar_low  = "       << barcntxt.bar_low
					//     << ", bar_close = "      << barcntxt.bar_close
					//     << ", bar_volume = "     << barcntxt.bar_volume
					//     << endl;
					handler->publishBar(barcntxt);
				}
			}
		}
		else {
			cout << "Publisher Thread => Timeout occured while reading bars data. No bars data to read" << endl;
		}
	}
}


//BACKUP
void *publisher_thread_publish_bars2(void *msg)
{
	struct pollfd fds[1];
	fds[0].fd = pfd_w2_w3[0];
	fds[0].events = POLLIN;
	BarCntxt barcntxt;

	cout << "Starting Seasocks server" << endl;

	//Seasocks logger
    auto logger = std::make_shared<PrintfLogger>(Logger::Level::Debug);

    Server server(logger);

    auto handler = std::make_shared<MyHandler>(&server);
    server.addWebSocketHandler("/", handler);
    //server.serve("src/Analytical_Server", 9090);
    server.startListening(9090);
	while (1) {
    	server.poll(100);
	}

	return nullptr;

	while(1) {
		int timeout_msecs = 2 * 1000;
		int ret = poll(fds, 1, timeout_msecs);

		if (ret > 0) {
			if (fds[0].revents & POLLIN) {

				if ( fcntl( fds[0].fd, F_SETFL, fcntl(fds[0].fd, F_GETFL) | O_NONBLOCK ) < 0 ) {
					cout << "Pubisher Thread => Error setting nonblocking flag for incoming bars data pipe" << endl;
					exit(0);
				}

				int r;
				while( (r = read(fds[0].fd, &barcntxt, sizeof(barcntxt))) > 0)  {

					string symbol(barcntxt.sym);
					//update the publishers bar cache
					auto it = pubs_bar_cache.find(symbol);
					bool bar_exists = ( it != pubs_bar_cache.end() );

					if (bar_exists == true) {
						//update existing entry in the publisher cache
						it->second = barcntxt;
					} else {
						//insert new entry in the publisher cache
						pubs_bar_cache.insert( pair<string, BarCntxt>(symbol, barcntxt) );
					}
					
					//TODO - Check the subscriptions and push the bar to subscribers thru appopriate client connection socket descriptors
					cout << "Publisher Thread => Read incoming bar : "
					     << "sym = "              << barcntxt.sym 
					     << ", bar_num = "        << barcntxt.bar_num
					     << ", bar_start_time = " << barcntxt.bar_start_time
					     << ", bar_close_time = " << barcntxt.bar_close_time
					     << ", bar_open = "       << barcntxt.bar_open
					     << ", bar_high = "       << barcntxt.bar_high
					     << ", bar_low  = "       << barcntxt.bar_low
					     << ", bar_close = "      << barcntxt.bar_close
					     << ", bar_volume = "     << barcntxt.bar_volume
					     << endl;
				}
			}
		}
		else {
			cout << "Publisher Thread => Timeout occured while reading bars data. No bars data to read" << endl;
		}
	}
}
