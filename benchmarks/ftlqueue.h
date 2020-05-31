#pragma once

#include <utility>
#include "lockfree_queue.h"
#include "wrappers.h"

template<typename T, bool bMultiProducer, bool bMultiConsumer>
struct FtlQueueWrapper
{
public:
	typedef DummyToken producer_token_t;
	typedef DummyToken consumer_token_t;
	
public:
    void init(std::size_t n)
	{
		q.init( n );
	}
	template<typename U>
	inline bool enqueue(U&& item)
	{
		q.push(std::forward<U>(item));
		return true;		// assume successful allocation for the sake of the benchmarks
	}
	
	inline bool try_dequeue(T& item)
	{
		return q.pop(&item);
	}
	
	// Dummy token methods (not used)
	bool enqueue(producer_token_t const&, T const&) { return false; }
	bool try_enqueue(producer_token_t, T const&) { return false; }
	bool try_dequeue(consumer_token_t, T& item) { return false; }
	template<typename It> bool enqueue_bulk(It, size_t) { return false; }
	template<typename It> bool enqueue_bulk(producer_token_t const&, It, size_t) { return false; }
	template<typename It> size_t try_dequeue_bulk(It, size_t) { return 0; }
	template<typename It> size_t try_dequeue_bulk(consumer_token_t, It, size_t) { return 0; }
	
private:
	ftl::LockfreeQueue<T, bMultiProducer, bMultiConsumer> q;
};

template<typename T>
using FtlMpmcQueueWrapper = FtlQueueWrapper<T, true, true>;

template<typename T>
using FtlMpscQueueWrapper = FtlQueueWrapper<T, true, false>;

template<typename T>
using FtlSpmcQueueWrapper = FtlQueueWrapper<T, false, true>;

template<typename T>
using FtlSpscQueueWrapper = FtlQueueWrapper<T, false, false>;
