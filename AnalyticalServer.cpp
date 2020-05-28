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

void *trade_data_reader(void *msg);
void *fsm_thread_bar_calc(void *msg);
vector<string> tokenize(const char *str, char c);
bool parse_trade(string line, map<string, string> & trdmap);

int pfd[2];

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
				}
			}
		}
		else {
			cout << "FSM Thread => Timeout occured while reading trade data. No trade data to read from source pipe fd" << endl;
		}
	}
}
