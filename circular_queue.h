/*
Copyright (C) 2011 by Peter Szücs

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

/*
Circular queue:
	-Cross-platform
	-High-performance
	-thread-safe
	-fair threading(oldest waiting thread first)
	-low memory usage (lower then normal queue)
	-without slow locks (mutex, semaphore)
	-blocked queue (automatic empty/full queue handling)
	-best for producer/consumer threading model
rules:
	-boost::thread, for cross-platform threading
	-set disable_interruption() on all threads.
	-you need to know/handle, how many items will be pushed.

example: circular_queue_example.cpp

interface:
	template <typename T, boost::uint32_t size = 32u>
	class circular_queue {
	public:
		void push(const T &item);
		void pushUnsafe(const T &item);
		void pop(T &item);
		T pop();
		void popUnsafe(T &item);
		T popUnsafe();
	};
*/

#ifndef CIRCULAR_QUEUE_H
#define CIRCULAR_QUEUE_H

// you will need to install boost: http://www.boost.org
#include <boost/cstdint.hpp> //uint32_t
#include <boost/interprocess/detail/atomic.hpp> //atomic_inc32()
#include <boost/thread.hpp> //yield()
#include <boost/exception/exception.hpp> //exception()

/*********/
/* Setup */
/*********/
//#ifdef _DEBUG
//#define CIRCULAR_QUEUE_DEBUG
//#endif

//disable functions, to save memory:
//#define CIRCULAR_QUEUE_DISABLE_SAFE_PUSH
//#define CIRCULAR_QUEUE_DISABLE_SAFE_POP

//yield will add CPU to other thread, but wont go sleep for fixed time.
//yield is not good for long living queues.
//#define CIRCULAR_QUEUE_WAIT() boost::this_thread::sleep(boost::posix_time::millisec(1))
#define CIRCULAR_QUEUE_WAIT() boost::this_thread::yield()

#ifdef CIRCULAR_QUEUE_DEBUG
	#include <iostream>
	#include <boost/interprocess/sync/interprocess_mutex.hpp>
	boost::interprocess::interprocess_mutex cout_mutex;
	#define COUT_WRITE(msg) { cout_mutex.lock(); std::cout << msg; cout_mutex.unlock();}
#endif

struct exNoMoreData : virtual boost::exception { };

template <typename T, boost::uint32_t size = 32u>
class circular_queue {
public:
	circular_queue() :
		mWritePos(0),
		mReadPos(0),
		mNoMorePush(false)
	{
		for(boost::uint32_t i = 0; i < size; i++){
			mHasData[i] = false;
			#ifndef CIRCULAR_QUEUE_DISABLE_SAFE_PUSH
				mPushQueue[i] = 0;
				mPushTicket[i] = 0;
			#endif
			#ifndef CIRCULAR_QUEUE_DISABLE_SAFE_POP
				mPopQueue[i] = 0;
				mPopTicket[i] = 0;
			#endif
		}

		// 0x100000000 needs to be dividable by size or it will fail on overflow.
		BOOST_STATIC_ASSERT( (0xFFFFFFFFu % size) == (size - 1) );
	}

	virtual ~circular_queue(){
		#ifdef CIRCULAR_QUEUE_DEBUG
			bool hasData = false;
			for(boost::uint32_t i = 0; i < size; i++){
				hasData |= mHasData[i];
			}
			if(hasData){
				//"Warning: Circular queue deleted, but it isn't empty!"
				BOOST_ASSERT(0);
			}
		#endif
	}

	//push item to the queue.
	//Use this, if you want to push only from single thread, but pop from multiple. (add tasks for workers)
	void pushUnsafe(const T &item){
		boost::uint32_t mypos = mWritePos;
		mWritePos++;
		
		#ifdef CIRCULAR_QUEUE_DEBUG
			COUT_WRITE("push " << mypos << std::endl);
		#endif
		mypos %= size;

		//queue is full, wait for workers.
		while(mHasData[mypos] != false){
			CIRCULAR_QUEUE_WAIT();
		}

		mData[mypos] = item;
		mHasData[mypos] = true;
	}
#ifndef CIRCULAR_QUEUE_DISABLE_SAFE_PUSH
	//push item to the queue.
	void push(const T &item){
		boost::uint32_t mypos = boost::interprocess::detail::atomic_inc32(&mWritePos);
		
		#ifdef CIRCULAR_QUEUE_DEBUG
			COUT_WRITE("push " << mypos << std::endl);
		#endif
		mypos %= size;
		boost::uint32_t nextpos = mypos % size;
		boost::uint32_t ticket = boost::interprocess::detail::atomic_inc32(&mPushQueue[mypos]);

		//another thread is pushing on the same queue item.
		//this is rare situation, you should increase size for speed-up, when this happens.
		while(ticket != mPushTicket[mypos]){
			CIRCULAR_QUEUE_WAIT();
		}

		//queue is full, wait for workers.
		while(mHasData[nextpos]){
			CIRCULAR_QUEUE_WAIT();
		}

		mData[mypos] = item;
		mHasData[mypos] = true;
		mPushTicket[mypos]++;
	}
#endif //CIRCULAR_QUEUE_DISABLE_SAFE_PUSH
#ifndef CIRCULAR_QUEUE_DISABLE_SAFE_POP
	//pop item from the queue and set item to it.
	bool pop(T &item){
		boost::uint32_t mypos = boost::interprocess::detail::atomic_inc32(&mReadPos);
		#ifdef CIRCULAR_QUEUE_DEBUG
		COUT_WRITE("pop " << mypos << std::endl);
		#endif
		mypos %= size;
		
		boost::uint32_t ticket = boost::interprocess::detail::atomic_inc32(&mPopQueue[mypos]);

		//another thread is popping on the same queue item.
		//this is rare situation, you should increase size for speed-up, when this happens.
		while(ticket != mPopTicket[mypos]){
			if(mNoMorePush)
				return false;
			CIRCULAR_QUEUE_WAIT();
		}

		//queue is empty, wait for data.
		while(!mHasData[mypos]){
			if(mNoMorePush)
				return false;
			CIRCULAR_QUEUE_WAIT();
		}
		
		item = mData[mypos];
		mHasData[mypos] = false;
		mPopTicket[mypos]++;
		return true;
	}

	//pop item from the queue and return it.
	T pop(){
		T data;
		if(pop(data))
			return data;
		else
			throw exNoMoreData();
	}
#endif //CIRCULAR_QUEUE_DISABLE_SAFE_POP

	//pop item from the queue and set item to it.
	//Use this, if you want to pop only from single thread, but push from multiple. (collect data)
	bool popUnsafe(T &item){
		boost::uint32_t mypos = mReadPos;
		mReadPos++;
		#ifdef CIRCULAR_QUEUE_DEBUG
			COUT_WRITE("pop " << mypos << std::endl);
		#endif
		mypos %= size;

		//queue is empty, wait for data.
		while(!mHasData[mypos]){
			if(mNoMorePush)
				return false;
			CIRCULAR_QUEUE_WAIT();
		}
		
		item = mData[mypos];
		mHasData[mypos] = false;
		return true;
	}
	//pop item from the queue and return it.
	//Use this, if you want to pop only from single thread, but push from multiple. (collect data)
	T popUnsafe(){
		T data;
		if(popUnsafe(data))
			return data;
		else
			throw exNoMoreData();

	}
	//when you don't want to push any data, you can call this, and all threads waiting for data will return a nullobject
	void signalNoMorePush(){
		mNoMorePush = true;
	}
private:
	//setting mData to volatile will disable some optimizations in T class.
	//All compilers (what I've tested) will set mData before mHasData,
	//so it should be safe without volatile.
	/* volatile */ T mData[size];
	volatile bool mHasData[size]; //signal from push to pop threads
	boost::uint32_t mWritePos;
	boost::uint32_t mReadPos;
	volatile bool mNoMorePush;

#ifndef CIRCULAR_QUEUE_DISABLE_SAFE_PUSH
	//ticket system works like a lock, but faster.
	//when a thread want to push:
	//	1. thread gets a ticket
	//	2. waits for threads ticket in mPushTicket
	//	3. waits for worker to process prev ticket.
	//	3. pushes data
	//	4. increases mPushTicket
	boost::uint32_t mPushQueue[size]; //get push ticket here
	volatile boost::uint32_t mPushTicket[size]; //current active push ticket
#endif
#ifndef CIRCULAR_QUEUE_DISABLE_SAFE_POP
	boost::uint32_t mPopQueue[size]; //get push ticket here
	volatile boost::uint32_t mPopTicket[size]; //current active push ticket
#endif
};

#endif //CIRCULAR_QUEUE_H
