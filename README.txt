Analytical Server
-----------------
The server runs three threads that have the following functions:

	1) Traded Data Reader (Worker 1) - Reads trade data from the trades.json file, constructs trade packets and sends to Worker 2 thru a pipe
	2) FSM Thread (Worker 2) - Recieves trade packets from Worker 1 and constructs 15 seconds OHLC bars. Emits bars data to Worker 3 (websockets) thread
	3) Websockets Publisher Thread (Worker 3) - Receives the bars from worker 2. Recieves websocket client connections and subscriptions. Pubilsh bars to subscribers based on subscriptions

Design criteria:
----------------
	The three threads consititute a pipline of processing stanges and are connected by simple unix pipes ( i.e pipe() )

	The reading end threads of the pipes / socket descritors use poll() call to wait for incoming data so that they don't block and can watch multiple descriptors if required.

	The system uses asynchronous logger g2log for easy logging across multiple threads

	The system uses seasocks websockets library to implement the websocket server to accept client connections and publich OHLC data

	Every thread maintians its own cache of incoming data. The FSM thread maintains a cache of OHLC BARS context it is currently working on.

	The Websockets thread maintains caches of connections and subscriptions. The websockets thread refer these caches while sending out OHLC bars to subscribers.

A sample bar context looks like this:

		struct BarCntxt {
		    char         sym[15];
		    unsigned int bar_num;
		    uint64_t     bar_start_time;
		    uint64_t     bar_close_time;
		    double       bar_open;
		    double       bar_high;
		    double       bar_low;
		    double       bar_close;
		    double       bar_volume;
		};

Following are the FSM (OHLC Bar constructing thred) states:

		enum FSM_States {  FSM_STARTING = 0,
		                   FSM_READY = 1,
		                   FSM_DOWN = 2,
		                   FSM_STATE_COUNT
		                };
		
The core logic of the FSM is driven by the FSM Function table that has pointers to functions that will be fired in response to events / state changes inside the FSM thread:

FSM_EVENT_HANDLER FSM_Ev_Handler_Table[FSM_STATE_COUNT][EVENT_TYPE_COUNT] = {
                                                                                process_fsm_starting, process_fsm_starting,
                                                                                process_fsm_ready_ev_trd_pkt_arrival, process_fsm_ready_ev_tmr_expiry,
                                                                                process_fsm_down, process_fsm_down
                                                                            };


Dependencies:
-------------
	1) seasocks - websocket libary
	2) g2log 	- asynchronous logging library

	To install seasocks:
		a) clone this git  :  https://github.com/mattgodbolt/seasocks
		b) build           :  $ mkdir build
		                      $ cd build
		                      $ cmake ..
		                      $ make
	
		More information on seasocks at https://github.com/mattgodbolt/seasocks/wiki/Seasocks-quick-tutorial

	To install g2log:
		a) clone this bitbucket reporitory: hg clone https://bitbucket.org/KjellKod/g2log
		b) build		: $ cd g2log
		                  $ mkdir build
		                  $ cd build 
		                  $ cmake ..
		                  $ make

Building the project:
---------------------

	
	1) Clone the project code from  https://github.com/ssyuva/StocksAnalyticalServer.git

	2) Cd into StocksAnalyticalServer directory. From inside the directory execute steps #3, #4

	3) Clone, build and install dependencies - $install_dependencies.sh

	4) Build the Analytical Sever project - source build.sh (make sure you source the file and not exeute it)

The build.sh will also set the LD_LIBRARY_PATH for the dependencies library
	
Running the project:
--------------------

	$ ./AnalyticalServer -h  will display help

	1) Start the server:  $ ./AnalyticalServer
	   The server will start listening for incoming websocket client connections in port 9090.
	   The server will wait 60 seconds initially without doing anything. Use this time to setup client subscriptions from other terminal windows as follows:

			$ wscat -c ws://localhost:9090
			> {"event": "subscribe", "symbol": "XETHXXBT", "interval" : "15"}
			> {"event": "subscribe", "symbol": "XETHZUSD", "interval" : "15"}
			> {"event": "subscribe", "symbol": "XXBTZUSD", "interval" : "15"}

			Whenever you send the client subscriptions, the server will respond back with a message stating the current set of tickers that particular client has subscribed for

	2) Establish client subscriptions from multiple terminal windows

	3) As soon as the initial wait time of 60 seconds is over, the server will start processing the trade file, constructing the OHLC bars and sending the same to clients. Keep a watch on the 
	   server terminal as well as client windows.

	4) The logs are found in the file. AnalyticalServer.g2log.*.log. You can watch (tail -f) this file to check what exactly is going on in the system.

	5) Sample subscriptions are available in the subscriptions.txt file. Use it to setup subscriptions.

Sample output at client end:
----------------------------

$ wscat -c ws://localhost:9090
connected (press CTRL+C to quit)
> {"event": "subscribe", "symbol": "ADAEUR", "interval" : "15"}
> {"event": "subscribe", "symbol": "ADAUSD", "interval" : "15"}
> {"event": "subscribe", "symbol": "ADAXBT", "interval" : "15"}
> {"event": "subscribe", "symbol": "BCHXBT", "interval" : "15"}
< Hello client! your current subscriptions : ADAEUR 
< Hello client! your current subscriptions : ADAEUR ADAUSD 
< Hello client! your current subscriptions : ADAEUR ADAUSD ADAXBT 
< Hello client! your current subscriptions : ADAEUR ADAUSD ADAXBT BCHXBT 
> {"event": "subscribe", "symbol": "DASHXBT", "interval" : "15"}
< Hello client! your current subscriptions : ADAEUR ADAUSD ADAXBT BCHXBT DASHXBT

< {"event": "ohlc_notify", "symbol": "DASHXBT", "bar_num": 6953, "O": 0.02783, "H": 0.02783, "L": 0.0278, "C": 0, "volume": 0.697432}
< {"event": "ohlc_notify", "symbol": "DASHXBT", "bar_num": 6953, "O": 0.02783, "H": 0.02783, "L": 0.0278, "C": 0, "volume": 1.05585}
< {"event": "ohlc_notify", "symbol": "ADAEUR", "bar_num": 7443, "O": 0.071799, "H": 0.071799, "L": 0.0717, "C": 0.0717, "volume": 9374.09}
< {"event": "ohlc_notify", "symbol": "ADAEUR", "bar_num": 7444, "O": 0.0717, "H": 0.0717, "L": 0.0717, "C": 0.0717, "volume": 0}
< {"event": "ohlc_notify", "symbol": "ADAUSD", "bar_num": 7422, "O": 0.082537, "H": 0.083, "L": 0.082537, "C": 0.083, "volume": 1445.78}

