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

using namespace std;

void *trade_data_reader(void *msg);
vector<string> tokenize(const char *str, char c);
bool parse_trade(string line, map<string, string> & trdmap);

int main()
{
	const char *tradefile = "trades.json";
	pthread_t trade_reader;

	int retval;

	retval = pthread_create(&trade_reader, NULL, trade_data_reader, (void *) tradefile);
	pthread_join(trade_reader, NULL);

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
		cout << "Stripped line: " << line << endl;

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
		
		cout << "Parsed Trade: " << "sym = " << symbol << ", P = " << price << ", Q = " << qty << ", TS2 = " << ts2 << endl;
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
