// Build 15 mins OHLC Bar chart data from trade data and publish the same thru websockets to clients
// Author: Yuvaraja Subramaniam ( www.linkedin.com/in/yuvaraja )

/*
	Thread 1: Reads the trade data from the json file
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

using namespace std;

//Trade Packet (Sent from Worker 1 to Worker 2)
struct tradepacket {
	char   sym[15];
	double price;
	double qty;
	uint64_t ts2;
};


//15 mins Bar Context
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
enum Bar_Type {CLOSING_BAR = 0, TRADE_BAR = 1, BAR_TYPE_COUNT};
vector<string> Bar_Type_Name = { "CLOSING_BAR", "TRADE_BAR", "BAR_TYPE_INVALID" };

//FSM States
enum FSM_States {FSM_STARTING = 0, FSM_READY = 1, FSM_DOWN = 2, FSM_STATE_COUNT};
vector<string> FSM_State_Name = { "FSM_STARTING", "FSM_READY", "FSM_DOWN", "FSM_STATE_INVALID" };


//Events processed by the FSM and the associated data
enum FSM_Event_Types {TRADE_PKT_ARRIVAL = 0, TIMER_EXPIRY = 1, EVENT_TYPE_COUNT};
vector<string> FSM_Event_Type_Name = { "TRADE_PKT_ARRIVAL", "TIMER_EXPIRY", "EVENT_TYPE_INVALID" };


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


int pfd[2];

FSM_States fsm_curr_state = FSM_STARTING;
map<string, BarCntxt> bar_cntxt_cache;

const uint64_t fifteen_min_millisecs = 15 * 60 * 1000 * 10 * 100 ;

int main()
{
	const char *tradefile = "trades.json";
	pthread_t trade_reader;
	pthread_t fsm_thread;

	//create the pipe for data sharing between worker 1 and worker 2
	pipe(pfd);

	int retval_1;

	retval_1 = pthread_create(&trade_reader, NULL, trade_data_reader, (void *) tradefile);

	int retval_2;
	const char *name = "FSM Thread";
	retval_2 = pthread_create(&fsm_thread, NULL, fsm_thread_bar_calc, (void *) name);

	pthread_join(trade_reader, NULL);
	pthread_join(fsm_thread, NULL);

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
		write(pfd[1], &tp, sizeof(tp));
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
	fds[0].fd = pfd[0];
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

					cout << "FSM Thread => read tradepacket : sym = " << symbol << ", P = " << price << ", Q = " << qty << ", TS2 = " << ts2 << endl;
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

	cout << "FSM Thread => event arrived = trd_pkt_arrival: sym = " << symbol << ", P = " << price << ", Q = " << qty << ", TS2 = " << ts2 << endl;

	//check if symbol exists in Bar contexts cache
	string sym(symbol);

	cout << "FSM Thread => Searching bar cache for symbol " << symbol << endl;

	auto it = bar_cntxt_cache.find(sym);

	bool cntxt_exists = (it != bar_cntxt_cache.end());

	cout << "FSM Thread => cntxt_exists = " << cntxt_exists << endl;

	if (!cntxt_exists) {
		//bars context does not exist. create it
	    cout << "FSM Thread => Bar context does not exist. Creating it : sym = " << symbol << endl;
		BarCntxt newcntxt;
		strcpy(newcntxt.sym, symbol);
		newcntxt.bar_num        = 1;
		newcntxt.bar_start_time = ts2;
		newcntxt.bar_close_time = ts2 + fifteen_min_millisecs;
		newcntxt.bar_open       = price;
		newcntxt.bar_high       = price;
		newcntxt.bar_low        = price;
		newcntxt.bar_close      = price;
		newcntxt.bar_volume     = qty;
	
		//store the new context in cache
		bar_cntxt_cache.insert( pair<string, BarCntxt>(sym, newcntxt) );
		//TODO - update subscribers on bar open
	}
	else {
		//bars context exist, update it
	    cout << "FSM Thread => Bar context exists. Update it : sym = " << symbol << endl;
		BarCntxt oldcntxt = it->second;
	    cout << "FSM Thread => sym = " << symbol << ", Current bar close time = " << oldcntxt.bar_close_time << ", Current TS2 : " << ts2 << endl;
		if ( ts2 <= oldcntxt.bar_close_time) {
	    cout << "FSM Thread => sym = " << symbol << ". Trade goes into exising bar" << endl;
		//TODO - remove debug
		fsm_emit_bar(oldcntxt, TRADE_BAR);

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
		fsm_emit_bar(oldcntxt, TRADE_BAR);

			//TODO - update subscribers on trade update
		}
		else {
			    //trade goes into next bar or someother future bar
	    		cout << "FSM Thread => sym = " << symbol << ". Trade goes into next bar or future bar" << endl;
				uint64_t curr_bar_close_time = oldcntxt.bar_close_time;
				do {
					// keep closing the current bar until the bar that accomodates the current trade opens up
					BarCntxt newcntxt;
					strcpy(newcntxt.sym, symbol);
					newcntxt.bar_num        = oldcntxt.bar_num + 1;
					newcntxt.bar_start_time = oldcntxt.bar_close_time + 1;
					newcntxt.bar_close_time = newcntxt.bar_start_time + fifteen_min_millisecs;
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
				fsm_emit_bar(oldcntxt, TRADE_BAR);
				//TODO - update subscribers on trade update
		}
	}
return true;
}


//Emit bar into to worker 3
bool fsm_emit_bar(BarCntxt barcntxt, Bar_Type bt){
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
	cout << "FSM Thread => Emiting Bar : " 
	             << "bartype = "          << Bar_Type_Name[bt] 
	             << ", symbol = "         << symbol 
	             << ", bar_num = "        << bar_num
	             << ", O = "              << bar_open
	             << ", H = "              << bar_high
	             << ", L = "              << bar_low
	             << ", C = "              << bar_close
	             << ", volume = "         << bar_volume
	             << ", bar_start_time = " << bar_start_time
	             << ", bar_close_time = " << bar_close_time
	             << endl;
}


//Process events TIMER_EXPIRY while FSM_State == FSM_READY
bool process_fsm_ready_ev_tmr_expiry(FSM_EVENT & fsm_ev) {
	uint64_t ts = fsm_ev.data.tmr_exp.ts; 
	cout << "FSM Thread => event arrived = timer_expiry: " << "TS = " << ts << endl;
return true;
}


//Process FSM Event - Demulitplex based on the fsm_curr_state and event type
bool process_fsm_event(FSM_EVENT & fsm_ev) {
	FSM_Event_Types ev_type = fsm_ev.type;	
	cout << "FSM Thread => dispatching event " << FSM_Event_Type_Name[ev_type] << " to handler function" << endl; 
	(*FSM_Ev_Handler_Table[fsm_curr_state][ev_type])(fsm_ev);

return true;
}
