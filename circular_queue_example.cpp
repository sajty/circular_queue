#include "circular_queue.h"
#include <boost/thread/thread.hpp>
#include <iostream>

//extreme test: pushing and popping 10million items,
//with 20 pushing and 20 popping thread(total of 40 threads) on a 16 item circular queue. :)
const int pushValue = 1;
const int taskCount = 10000000;
const int pushingThreadCount = 20;
const int poppingThreadCount = 20;

circular_queue<int, 16u> tasks;
int result[poppingThreadCount];
void pushData(int id){
	boost::this_thread::disable_interruption di;
	int count = taskCount/pushingThreadCount;

	// when taskCount is not dividable with pushingThreadCount,
	// then some threads needs to run once more.
	if(taskCount%pushingThreadCount>id)
		count++;

	for(int i=0;i<count;i++)
		tasks.push(pushValue);

}
void popData(int id){
	boost::this_thread::disable_interruption di;
	try {
		while(1)
			result[id] += tasks.pop();
	} catch(exNoMorePush ex){}
}
int main(){
	boost::thread* pushingThreads[pushingThreadCount];
	boost::thread* poppingThreads[poppingThreadCount];

	for(int i=0;i<pushingThreadCount;i++){
		pushingThreads[i] = new boost::thread(boost::bind(pushData,i));
	}
	for(int i=0;i<poppingThreadCount;i++){
		poppingThreads[i] = new boost::thread(boost::bind(popData,i));
	}

	for(int i=0;i<pushingThreadCount;i++){
		pushingThreads[i]->join();
		std::cout << "pushing thread " << i << " completed!" << std::endl;
	}
	tasks.signalNoMorePush();

	int final_result=0;
	for(int i=0;i<poppingThreadCount;i++){
		poppingThreads[i]->join();
		final_result += result[i];
		std::cout << "popping thread " << i << " completed: " << result[i] << " elements popped" <<std::endl;
	}
	
	std::cout << "Value should be:  " << pushValue * taskCount << std::endl;
	std::cout << "Calculated value: " << final_result << std::endl;
	std::cin.get();
}
