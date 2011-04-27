/**
 * @file
 * @author Peter Szucs <peter.szucs.dev@gmail.com>
 *
 * @section LICENSE
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * @class circular_queue
 * \brief High-performance, thread-safe, cross-platform circular queue for producer/consumer threading models.
 * 
 * Queue operations:<ul>
 * 	<li>push: you can add item to the end of a list</li>
 * 	<li>pop: you can remove first item from list and return it</li>
 * </ul>
 * Circular queue:<ul>
 * 	 <li>Cross-platform</li>
 * 	 <li>High-performance</li>
 * 	 <li>Thread-safe</li>
 * 	 <li>Fair threading(oldest waiting thread first)</li>
 * 	 <li>Low memory usage (lower then normal queue)</li>
 * 	 <li>Without slow locks (mutex, semaphore)</li>
 * 	 <li>Blocked queue (automatic empty/full queue handling)</li>
 * 	 <li>Best for producer/consumer threading model</li>
 * </ul>
 * rules:<ul>
 * 	<li>Use boost::thread, for cross-platform threading</li>
 * 	<li>Set disable_interruption() on all threads</li>
 * 	<li>You need to know/handle, how many items will be pushed</li>
 *  </ul>
 *
 * example: see circular_queue_example.cpp
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
#ifdef _DEBUG
	// Will write to cout every push and pop operation.
	//#define CIRCULAR_QUEUE_VERBOSE
	// This will check in destructor, that the queue is empty.
	//#define CIRCULAR_QUEUE_SAFE_DELETE
#endif
// When you only use pushUnsafe(), you can disable push() to save memory.
//#define CIRCULAR_QUEUE_DISABLE_SAFE_PUSH
// When you only use popUnsafe(), you can disable pop() to save memory.
//#define CIRCULAR_QUEUE_DISABLE_SAFE_POP

// This is called, when the thread needs to wait
#ifndef CIRCULAR_QUEUE_WAIT
	// yield will add CPU to other thread, but wont go sleep for fixed time.
	// yield is not good for long living queues.
	#define CIRCULAR_QUEUE_WAIT() boost::this_thread::yield()
	
	//#define CIRCULAR_QUEUE_WAIT() boost::this_thread::sleep(boost::posix_time::millisec(1))
#endif

// End of Setup

#ifdef CIRCULAR_QUEUE_VERBOSE
	#include <iostream>
	#include <boost/interprocess/sync/interprocess_mutex.hpp>
	boost::interprocess::interprocess_mutex cout_mutex;
	#define COUT_WRITE(msg) { cout_mutex.lock(); std::cout << msg; cout_mutex.unlock();}
#endif

//! Exception, throwed in pop() and popUnsafe(), when queue is empty and after signalNoMorePush() is called.
struct exNoMorePush : virtual boost::exception { };



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
		#ifdef CIRCULAR_QUEUE_SAFE_DELETE
			bool hasData = false;
			for(boost::uint32_t i = 0; i < size; i++){
				hasData |= mHasData[i];
			}
			//asserts, when you delete a non-empty queue.
			//you can disable this assert by defining CIRCULAR_QUEUE_SAFE_DELETE
			BOOST_ASSERT(!hasData)
		#endif
	}
	
	/*! \brief Push item to queue without push thread-safety.
	 * 
	 * Use this for single-threaded pushing and multi-threaded popping. (add tasks for workers)
	 * 
	 * @param item The item to push to the queue.
	 */
	void pushUnsafe(const T &item){
		BOOST_ASSERT(!mNoMorePush);
		boost::uint32_t mypos = mWritePos;
		mWritePos++;
		
		#ifdef CIRCULAR_QUEUE_VERBOSE
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
	/*! \brief Push item to queue.
	 * 
	 * Thread-safe push.
	 * 
	 * @param item The item to push to the queue.
	 */
	void push(const T &item){
		BOOST_ASSERT(!mNoMorePush);
		boost::uint32_t mypos = boost::interprocess::detail::atomic_inc32(&mWritePos);
		
		#ifdef CIRCULAR_QUEUE_VERBOSE
			COUT_WRITE("push " << mypos << std::endl);
		#endif
		mypos %= size;
		
		boost::uint32_t ticket = boost::interprocess::detail::atomic_inc32(&mPushQueue[mypos]);

		//another thread is pushing on the same queue item.
		//happens, when a thread is doing a push() and the cpu is switched to other thread, which pushes 32 items, before the other thread can do the push.
		//this is rare situation, you should increase size for speed-up, when this happens.
		while(ticket != mPushTicket[mypos]){
			CIRCULAR_QUEUE_WAIT();
		}

		//queue is full, wait for workers.
		while(mHasData[mypos]){
			CIRCULAR_QUEUE_WAIT();
		}

		mData[mypos] = item;
		mHasData[mypos] = true;
		mPushTicket[mypos]++;
	}
#endif //CIRCULAR_QUEUE_DISABLE_SAFE_PUSH
#ifndef CIRCULAR_QUEUE_DISABLE_SAFE_POP
	/*! \brief Pop item from queue and return the popped item.
	 * 
	 * Thread-safe pop.
	 * 
	 * @param item Item, where the popped item will be copied.
	 * @return True, when success. False, when queue is empty and signalNoMorePush() was called.
	 */
	bool pop(T &item){
		boost::uint32_t mypos = boost::interprocess::detail::atomic_inc32(&mReadPos);
		#ifdef CIRCULAR_QUEUE_VERBOSE
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

	/*! \brief Pop item from queue and return the popped item.
	 * 
	 * Thread-safe pop.
	 * Will throw exNoMorePush exception, when queue is empty and signalNoMorePush() was called.
	 * 
	 * @return The item popped.
	 */
	T pop(){
		T data;
		if(pop(data))
			return data;
		else
			throw exNoMorePush();
	}
#endif //CIRCULAR_QUEUE_DISABLE_SAFE_POP

	/*! \brief Pop item from queue and return the popped item.
	 * 
	 * Use this, if you want to pop only from single thread, but push from multiple. (collect data)
	 * 
	 * @param item Item, where the popped item will be copied.
	 * @return True, when success. False, when queue is empty and signalNoMorePush() was called.
	 */
	bool popUnsafe(T &item){
		boost::uint32_t mypos = mReadPos;
		mReadPos++;
		#ifdef CIRCULAR_QUEUE_VERBOSE
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
	/*! \brief Pop item from queue and return the popped item.
	 * 
	 * Use this, if you want to pop only from single thread, but push from multiple. (collect data)
	 * Will throw exNoMorePush exception, when queue is empty and signalNoMorePush() was called.
	 * 
	 * @return The item popped.
	 */
	T popUnsafe(){
		T data;
		if(popUnsafe(data))
			return data;
		else
			throw exNoMorePush();
	}
	/*! \brief Close the queue for pushing.
	 * 
	 * When you don't want to push any more data, you can call this, and all threads waiting for data will return.
	 * 
	 */
	void signalNoMorePush(){
		mNoMorePush = true;
	}
	/*! \brief Gets the estimated length of the queue
	 * 
	 * You can use this for checking when new data is availible, but only in single-threaded popping with popUnsafe().
	 * When pop threads are waiting for data, it will be negative.
	 * When push threads are waiting in a full queue, it will be bigger then size. 
	 */
	int getQueueLength(){
		return (int)(mWritePos - mReadPos);
	}
private:
	/* Contains the queue items.
	 * Setting mData to volatile will disable some optimizations in T class.
	 * All compilers (what I've tested) will set mData before mHasData,
	 * so it should be safe without volatile.
	 */
	/* volatile */ T mData[size]; // queue items
	volatile bool mHasData[size]; // signal between push and pop threads
	volatile boost::uint32_t mWritePos; // push position
	volatile boost::uint32_t mReadPos; //pop position
	volatile bool mNoMorePush; //

#ifndef CIRCULAR_QUEUE_DISABLE_SAFE_PUSH
	//ticket system works like a lock, but faster.
	//when a thread want to push:
	//	1. thread gets a ticket
	//	2. waits for threads ticket in mPushTicket
	//	3. waits for worker to process prev ticket.
	//	3. pushes data
	//	4. increases mPushTicket
	volatile boost::uint32_t mPushQueue[size]; //get push ticket here
	volatile boost::uint32_t mPushTicket[size]; //current active push ticket
#endif
#ifndef CIRCULAR_QUEUE_DISABLE_SAFE_POP
	volatile boost::uint32_t mPopQueue[size]; //get push ticket here
	volatile boost::uint32_t mPopTicket[size]; //current active push ticket
#endif
};

//doxygen needs them defined, to include it in documentation.
#ifdef DOXYGEN
	//! Will write to cout every push and pop operation.
	#define CIRCULAR_QUEUE_VERBOSE
	
	//! This will check in destructor, that the queue is empty.
	#define CIRCULAR_QUEUE_SAFE_DELETE
	
	//! When you only use pushUnsafe(), you can disable push() to save memory.
	#define CIRCULAR_QUEUE_DISABLE_SAFE_PUSH
	
	//! When you only use popUnsafe(), you can disable pop() to save memory.
	#define CIRCULAR_QUEUE_DISABLE_SAFE_POP
	
	//!This is called, when the thread needs to wait
	#define CIRCULAR_QUEUE_WAIT()
#endif

#endif //CIRCULAR_QUEUE_H
