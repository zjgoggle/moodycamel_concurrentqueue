/*
 * This file is part of the ftl (Fast Template Library) distribution (https://github.com/zjgoggle/ftl).
 * Copyright (c) 2020 Jack Zhang.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <atomic>
#include <cassert>
#include <memory>

namespace ftl
{

namespace internal
{

    inline uint64_t previous_power2_inclusive( uint64_t x )
    {
        if ( x == 0 )
            return 0;
#ifdef __GNUC__
        return 1 << ( 63 - __builtin_clzl( x ) ); // count leading zeros.
#else
        // x--; Uncomment this, if you want a strictly less than 'x' result.
        x |= ( x >> 1 );
        x |= ( x >> 2 );
        x |= ( x >> 4 );
        x |= ( x >> 8 );
        x |= ( x >> 16 );
        x |= ( x >> 32 );
        return x - ( x >> 1 );
#endif
    }
    inline uint32_t LockfreeQueue( uint32_t x )
    {
        if ( x == 0 )
            return 0;
#ifdef __GNUC__
        return 1 << ( 31 - __builtin_clz( x ) ); // count leading zeros.
#else
        // x--; Uncomment this, if you want a strictly less than 'x' result.
        x |= ( x >> 1 );
        x |= ( x >> 2 );
        x |= ( x >> 4 );
        x |= ( x >> 8 );
        x |= ( x >> 16 );
        return x - ( x >> 1 );
#endif
    }
    inline uint32_t next_power2_inclusive( uint32_t x )
    {
        x = x - 1;
        x |= ( x >> 1 );
        x |= ( x >> 2 );
        x |= ( x >> 4 );
        x |= ( x >> 8 );
        x |= ( x >> 16 );
        return x + 1;
    }
    inline uint64_t next_power2_inclusive( uint64_t x )
    {
        x = x - 1;
        x |= ( x >> 1 );
        x |= ( x >> 2 );
        x |= ( x >> 4 );
        x |= ( x >> 8 );
        x |= ( x >> 16 );
        x |= ( x >> 32 );
        return x + 1;
    }

} // namespace internal

template<class T, bool bMultiProcuer, bool MultiConsumer, class AllocT = std::allocator<T>, std::size_t AvoidFalseSharingSize = 128>
class LockfreeQueue;


////////////////////////////////////////////////////////////////////////////////////////////
///                      SPSCCircularQueue
////////////////////////////////////////////////////////////////////////////////////////////

template<class T, class AllocT, std::size_t AvoidFalseSharingSize>
class LockfreeQueue<T, false, false, AllocT, AvoidFalseSharingSize>
{
    AllocT mAlloc;
    size_t m_nEntriesMinus1 = 0;
    T *mBuf = nullptr;
    char m_padding1[AvoidFalseSharingSize - sizeof( AllocT ) - sizeof( size_t ) - sizeof( T * )];

    std::atomic<size_t> mPushPos{0};
    char m_padding2[AvoidFalseSharingSize - sizeof( std::atomic<size_t> )];

    std::atomic<size_t> mPopPos{0};
    char m_padding3[AvoidFalseSharingSize - sizeof( std::atomic<size_t> )];

public:
    constexpr static bool support_multiple_producer_threads = false, support_multiple_consumer_threads = false;

    LockfreeQueue &operator=( const LockfreeQueue & ) = delete;

public:
    using value_type = T;

    LockfreeQueue( size_t cap = 0, const AllocT &alloc = AllocT() ) : mAlloc( alloc )
    {
        if ( cap )
            init( cap );
    }

    LockfreeQueue( LockfreeQueue &&a ) : mAlloc( a.mAlloc ), m_nEntriesMinus1( a.m_nEntriesMinus1 ), mBuf( a.mBuf )
    {
        mPushPos = a.mPushPos.load();
        mPopPos = a.mPopPos.load();
        a.mBuf = nullptr;
        a.mPushPos = 0;
        a.mPopPos = 0;
        if ( mBuf == nullptr && m_nEntriesMinus1 > 0 )
            init( m_nEntriesMinus1 + 1 );
    }

    LockfreeQueue( const LockfreeQueue &a ) : mAlloc( a.alloc )
    {
        init( a.m_nEntriesMinus1 + 1 );
    }

    bool init( size_t cap )
    {
        assert( cap > 1 && ( cap & ( cap - 1 ) ) == 0 ); // power 2
        destruct();
        cap = internal::next_power2_inclusive( cap + 1 ); // plus 1 for the senitel in SPSC
        m_nEntriesMinus1 = cap - 1;
        mPushPos = 0;
        mPopPos = 0;
        if ( m_nEntriesMinus1 > 0 )
        {
            mBuf = mAlloc.allocate( cap );
            return mBuf;
        }
        return true;
    }

    ~LockfreeQueue()
    {
        destruct();
    }

    void destruct()
    {
        if ( mBuf )
        {
            mAlloc.deallocate( mBuf, m_nEntriesMinus1 + 1 );
            mBuf = nullptr;
        }
    }

    size_t capacity() const
    {
        return m_nEntriesMinus1; //
    }
    bool full() const
    {
        return m_nEntriesMinus1 == 0 ||
               ( ( mPushPos.load( std::memory_order_relaxed ) + 1 ) & m_nEntriesMinus1 ) == mPopPos.load( std::memory_order_relaxed );
    }
    bool empty() const
    {
        return m_nEntriesMinus1 == 0 || mPushPos.load( std::memory_order_relaxed ) == mPopPos.load( std::memory_order_relaxed );
    }
    size_t size() const
    {
        auto pushpos = mPushPos.load( std::memory_order_relaxed ) & m_nEntriesMinus1;
        auto poppos = mPopPos.load( std::memory_order_relaxed ) & m_nEntriesMinus1;
        return pushpos >= poppos ? ( pushpos - poppos ) : ( m_nEntriesMinus1 + 1 - poppos + pushpos );
    }

    // get the position for push operation.
    T *prepare_push() const
    {
        if ( !m_nEntriesMinus1 )
            return nullptr;
        auto pushpos = mPushPos.load( std::memory_order_relaxed ) & m_nEntriesMinus1;
        auto poppos = mPopPos.load( std::memory_order_relaxed ) & m_nEntriesMinus1;
        if ( ( ( pushpos + 1 ) & m_nEntriesMinus1 ) == poppos ) // full
            return nullptr;
        return mBuf + pushpos;
    }
    // prepare_push must be called before commit_push
    void commit_push()
    {
        mPushPos.fetch_add( 1, std::memory_order_acq_rel );
    }

    template<class... Args>
    T *emplace( Args &&... args )
    {
        if ( auto p = prepare_push() )
        {
            new ( p ) T( std::forward<Args>( args )... );
            commit_push();
            return p;
        }
        return nullptr;
    }
    T *push( const T &val )
    {
        return emplace( val );
    }

    T *top() const
    {
        if ( !m_nEntriesMinus1 )
            return nullptr;
        auto pushpos = mPushPos.load( std::memory_order_relaxed ) & m_nEntriesMinus1;
        auto poppos = mPopPos.load( std::memory_order_relaxed ) & m_nEntriesMinus1;
        if ( pushpos == poppos ) // empty
            return nullptr;
        return &mBuf[poppos];
    }
    // v: uninitialized memory
    bool pop( T *buf = nullptr )
    {
        if ( !m_nEntriesMinus1 )
            return false;
        auto pushpos = mPushPos.load( std::memory_order_relaxed ) & m_nEntriesMinus1;
        auto poppos = mPopPos.load( std::memory_order_relaxed ) & m_nEntriesMinus1;
        if ( pushpos == poppos ) // empty
            return false;
        if ( buf )
            new ( buf ) T( std::move( mBuf[poppos] ) );
        mBuf[poppos].~T();
        mPopPos.fetch_add( 1, std::memory_order_acq_rel );
        return true;
    }
};

////////////////////////////////////////////////////////////////////////////////////////////
///                      SPMCCircularQueue
////////////////////////////////////////////////////////////////////////////////////////////

template<class T, class AllocT, std::size_t AvoidFalseSharingSize>
class LockfreeQueue<T, false, true, AllocT, AvoidFalseSharingSize>
{
    struct Node
    {
        std::atomic<bool> flag; // true for having value
        T val;
    };
    using Alloc = typename std::allocator_traits<AllocT>::template rebind_alloc<Node>;

    Alloc m_alloc;
    Node *m_data = nullptr;
    size_t m_nEntriesMinus1 = 0;
    char m_padding1[std::max( 0UL, AvoidFalseSharingSize - sizeof( Alloc ) - sizeof( Node * ) - sizeof( size_t ) )];

    std::atomic<size_t> m_begin{0};
    char m_padding2[AvoidFalseSharingSize - sizeof( std::atomic<size_t> )];

    std::atomic<size_t> m_end{0};
    char m_padding3[AvoidFalseSharingSize - sizeof( std::atomic<size_t> )];

public:
    using value_type = T;
    constexpr static bool support_multiple_producer_threads = false, support_multiple_consumer_threads = true;

    LockfreeQueue( size_t nCap = 0, const Alloc &alloc = Alloc() ) : m_alloc( alloc )
    {
        if ( nCap )
            init( nCap );
    }

    ~LockfreeQueue()
    {
        if ( m_data )
        {
            m_alloc.deallocate( m_data, m_nEntriesMinus1 + 1 );
            m_data = nullptr;
        }
    }

    bool init( size_t nCap )
    {
        assert( nCap > 1 && ( nCap & ( nCap - 1 ) ) == 0 ); // power 2
        if ( nCap < 2 )
            return false;
        if ( m_data )
        {
            m_alloc.deallocate( m_data, m_nEntriesMinus1 + 1 );
            m_data = nullptr;
        }
        nCap = internal::next_power2_inclusive( nCap );
        m_data = m_alloc.allocate( nCap );
        m_nEntriesMinus1 = nCap - 1;
        for ( size_t i = 0; i < nCap; ++i )
            m_data[i].flag.store( false );
        return m_data;
    }

    /// @return false when full.
    template<class... Args>
    bool emplace( Args &&... args )
    {
        auto iEnd = m_end.load( std::memory_order_relaxed ) & m_nEntriesMinus1;
        if ( m_data[iEnd].flag.load( std::memory_order_acquire ) ) // full
            return false;
        new ( &m_data[iEnd].val ) T( std::forward<Args>( args )... );
        assert( !m_data[iEnd].flag );
        m_data[iEnd].flag.store( true, std::memory_order_release );
        m_end.fetch_add( 1, std::memory_order_acq_rel );
        return true;
    }

    bool push( const T &val )
    {
        return emplace( val );
    }

    bool pop( T *val = nullptr, const std::size_t maxtry = 10000ul )
    {
        size_t ibegin = m_begin.load( std::memory_order_acquire );
        for ( auto i = 0ul; i < maxtry; ++i )
        {
            if ( ibegin == m_end.load( std::memory_order_acquire ) ) // emtpy
                return false;
            if ( m_begin.compare_exchange_weak( ibegin, ibegin + 1, std::memory_order_relaxed ) )
            {
                // alreay acquired the ownership.
                ibegin &= m_nEntriesMinus1;
                auto &data = m_data[ibegin];
                assert( data.flag.load( std::memory_order_acquire ) );

                if ( val )
                    new ( val ) T( std::move( data.val ) );
                data.val.~T();
                data.flag.store( false, std::memory_order_release );
                return true;
            }
        }
        return false;
    }

    std::size_t size() const
    {
        auto poppos = m_begin.load( std::memory_order_relaxed ) & m_nEntriesMinus1;
        auto pushpos = m_end.load( std::memory_order_relaxed ) & m_nEntriesMinus1;
        if ( pushpos < poppos )
            return m_nEntriesMinus1 + 1 - ( poppos - pushpos );
        return pushpos - poppos;
    }

    // dequeue the last element may be in process.
    bool empty() const
    {
        return m_begin.load() == m_end.load();
    }

    bool full() const
    {
        auto iEnd = m_end.load( std::memory_order_acquire ) & m_nEntriesMinus1;
        return ( m_data[iEnd].flag.load( std::memory_order_relaxed ) ); // full
    }

    void clear()
    {
        while ( pop( nullptr ) )
            ;
    }
};

////////////////////////////////////////////////////////////////////////////////////////////
///                      MPSCCircularQueueBase
////////////////////////////////////////////////////////////////////////////////////////////


template<class T, class AllocT, std::size_t AvoidFalseSharingSize>
class LockfreeQueue<T, true, false, AllocT, AvoidFalseSharingSize>
{
public:
    constexpr static bool support_multiple_producer_threads = true, support_multiple_consumer_threads = false;

    using size_type = std::size_t;
    using seq_type = std::atomic<size_type>;
    struct Entry
    {
        seq_type mSeq; // padding for alignment
        T mData;

        seq_type &seq()
        {
            return *reinterpret_cast<seq_type *>( &mSeq );
        }
        T &data()
        {
            return *reinterpret_cast<T *>( &mData );
        }
    };
    using allocator_type = AllocT;
    using node_allocator = typename std::allocator_traits<AllocT>::template rebind_alloc<Entry>;

protected:
    node_allocator mAlloc;
    size_type m_nEntriesMinus1 = 0;
    Entry *mBuf = nullptr;
    char m_padding1[std::max( 0UL, AvoidFalseSharingSize - sizeof( mAlloc ) - sizeof( std::size_t ) - sizeof( void * ) )];

    seq_type mPushPos = 0;
    char m_padding2[std::max( 0UL, AvoidFalseSharingSize - sizeof( seq_type ) )];

    seq_type mPopPos = 0;
    char m_padding3[AvoidFalseSharingSize - sizeof( seq_type )];

public:
    using value_type = T;

    LockfreeQueue( size_t cap = 0, const allocator_type &alloc = allocator_type{} ) : mAlloc( alloc )
    {
        if ( cap )
            init( cap );
    }

    ~LockfreeQueue()
    {

        if ( mBuf )
        {
            mAlloc.deallocate( mBuf, m_nEntriesMinus1 + 1 );
            mBuf = nullptr;
        }
    }

    /// @brief Move Constructor: move memory if it's allocated. Otherwise, allocate new buffer.
    LockfreeQueue( LockfreeQueue &&a )
        : mAlloc( std::move( a.mAlloc ) ),
          m_nEntriesMinus1( a.m_nEntriesMinus1 ),
          mBuf( a.mBuf ),
          mPushPos( a.mPushPos.load() ),
          mPopPos( a.mPopPos.load() )
    {
        a.mBuf = nullptr;
        if ( mBuf == nullptr && m_nEntriesMinus1 > 0 )
            init( m_nEntriesMinus1 );
    }

    /// @brief Copy Constructor: alway allocate new buffer.
    LockfreeQueue( const LockfreeQueue &a ) : LockfreeQueue( a.mCap, a.mAlloc )
    {
    }

    LockfreeQueue &operator=( const LockfreeQueue &a )
    {
        this->~MPSCBoundedQueue();
        new ( this ) LockfreeQueue( a );
        return *this;
    }

    LockfreeQueue &operator=( LockfreeQueue &&a )
    {
        this->~LockfreeQueue();
        new ( this ) LockfreeQueue( std::move( a ) );
        return *this;
    }

    bool init( size_t cap )
    {
        assert( cap && ( cap & ( cap - 1 ) ) == 0 ); // power 2
        if ( mBuf )
        {
            mAlloc.deallocate( mBuf, m_nEntriesMinus1 + 1 );
            mBuf = nullptr;
        }
        cap = internal::next_power2_inclusive( cap );
        m_nEntriesMinus1 = cap - 1;
        mBuf = mAlloc.allocate( cap );
        if ( !mBuf )
            return false;
        for ( size_t i = 0; i < cap; ++i )
        {
            new ( &mBuf[i].seq() ) seq_type();
            mBuf[i].seq() = i;
        }
        mPushPos = 0;
        mPopPos = 0;
        return true;
    }

    /// \brief emplace back.
    /// \return false if it's full.
    template<class... Args>
    bool emplace( Args &&... args )
    {
        return emplace_retry( std::numeric_limits<std::size_t>::max(), std::forward<Args>( args )... );
    }

    template<class... Args>
    bool emplace_retry( const std::size_t maxtry = 1000000ul, Args &&... args )
    {
        for ( auto i = 0ul; i < maxtry; ++i )
        {
            auto pushpos = mPushPos.load( std::memory_order_relaxed );
            auto &entry = mBuf[pushpos & m_nEntriesMinus1];
            auto seq = entry.seq().load( std::memory_order_acquire );

            if ( seq == pushpos )
            { // acquired pushpos & seq
                if ( mPushPos.compare_exchange_weak( pushpos, pushpos + 1, std::memory_order_relaxed ) )
                {
                    new ( &entry.data() ) T( std::forward<Args>( args )... );
                    entry.seq().store( seq + 1, std::memory_order_release ); // inc seq. when popping, seq == poppos+1
                    return true;
                } // else pushpos was acquired by other push thread, retry
            }
            else if ( seq == pushpos + 1 ) // seq was acquired by other producer before loading seq.
            {
                continue;
            }
            else if ( seq + m_nEntriesMinus1 == pushpos ) //  full: iseq - 1 +  CAPACITY == iprod
            {
                return false;
            }
            else if ( seq == pushpos + 1 + m_nEntriesMinus1 ) //  extremly unlikely, entry is consumed between fetching iprod and iseq. Retry it.
            {
                continue;
            }
            else
            {
                assert( false && "program logic error!" );
                throw std::runtime_error( std::string( "MPSC push. Unexpected produce pos:" ) + std::to_string( pushpos ) +
                                          ", iseq:" + std::to_string( seq ) + ", entries:" + std::to_string( m_nEntriesMinus1 + 1 ) );
            }
        }
        return false;
    }

    /// \brief push back
    bool push( const T &v, const std::size_t maxtry = 1000000ul )
    {
        return emplace_retry( maxtry, v );
    }

    /// \brief pop back.
    /// buf: uninitialized memory
    bool pop( T *buf = nullptr )
    {
        auto poppos = mPopPos.load( std::memory_order_acquire );
        auto &entry = mBuf[poppos & m_nEntriesMinus1];
        auto seq = entry.seq().load( std::memory_order_acquire );

        if ( poppos + 1 == seq )
        {
            mPopPos.store( poppos + 1, std::memory_order_relaxed );
            if ( buf )
                new ( buf ) T( std::move( entry.data() ) );
            entry.data().~T();
            entry.seq().store( seq + m_nEntriesMinus1, std::memory_order_release ); // dec seq and plus mCap. when next push, seq == pushpos.
            return true;
        }
        else
        { // empty
            //            assert( seq == poppos && "program logic error!" );
            if ( seq != poppos )
                throw std::runtime_error( std::string( "MPSC pop. Unexpected consume pos:" ) + std::to_string( poppos ) +
                                          ", iseq:" + std::to_string( seq ) + ", iProd:" + std::to_string( mPushPos.load() ) +
                                          ", entries:" + std::to_string( m_nEntriesMinus1 + 1 ) );
            return false;
        }
    }

    T *top() const
    {
        auto poppos = mPopPos.load( std::memory_order_acquire );
        auto &entry = mBuf[poppos & m_nEntriesMinus1];
        auto seq = entry.seq().load( std::memory_order_relaxed );

        if ( poppos + 1 == seq )
        {
            return &entry.data();
        }
        else
        { // empty
            assert( seq == poppos );
            if ( seq != poppos )
                throw std::runtime_error( std::string( "Unexpected consume pos:" ) + std::to_string( poppos ) + ", iseq:" + std::to_string( seq ) +
                                          ", entries:" + std::to_string( m_nEntriesMinus1 + 1 ) );
            return nullptr;
        }
    }

    std::size_t size() const
    {
        auto poppos = mPopPos.load( std::memory_order_relaxed ) & m_nEntriesMinus1;
        auto pushpos = mPushPos.load( std::memory_order_relaxed ) & m_nEntriesMinus1;
        if ( pushpos < poppos )
            return m_nEntriesMinus1 + 1 - ( poppos - pushpos );
        return pushpos - poppos;
    }

    bool empty() const
    {
        auto poppos = mPopPos.load( std::memory_order_acquire );
        return mBuf[poppos & m_nEntriesMinus1].seq().load( std::memory_order_relaxed ) == poppos;
    }
    // non-strictly full, enqueue the last entry my be in process.
    bool full() const
    {
        auto pushpos = mPushPos.load( std::memory_order_acquire );
        return mBuf[pushpos & m_nEntriesMinus1].seq().load( std::memory_order_relaxed ) + 2 + m_nEntriesMinus1 == pushpos;
    }
    void clear()
    {
        while ( pop( nullptr ) )
            ;
    }
};

////////////////////////////////////////////////////////////////////////////////////////////
///                      MPMCCircularQueueBase
////////////////////////////////////////////////////////////////////////////////////////////

/// \brief class MPMCCircularQueueBase
/// Initially, for each entry, seq = index. seq == index (mod CAPACITY) if entry has no value.
/// When producer acquires an entry, it increases seq by 1. So seq +1 == index (mod CAPACITY) if entry has value.
/// When consumer acquires an entry, it increases seq by CAPCACITY-1, seq is index + nConsumption * CAPCACITY, where nConsumption is the times of
/// consumptions.
/// \note it only supports power of 2 nnumber of entries.
template<class T, std::size_t AvoidFalseSharingSize = 128>
class MPMCCircularQueueBase
{
public:
    using value_type = T;
    using size_type = unsigned;
    struct Entry
    {
        std::atomic<size_type> seq;
        char data[sizeof( T )]; // todo padding.

        void init( size_type seqNo )
        {
            seq = seqNo;
        }
        T *getObj()
        {
            return reinterpret_cast<T *>( data );
        }
    };

protected:
    size_type m_nEntriesMinus1; // nEntries -1
    Entry *m_data = nullptr;
    char m_padding0[AvoidFalseSharingSize - sizeof( size_type ) - sizeof( Entry * )];

    //    std::atomic<size_type> m_iProduceCommitted{0};
    //    char m_padding1[AvoidFalseSharingSize - sizeof( std::atomic<size_type> )];

    std::atomic<size_type> m_iConsume{0};
    char m_padding2[AvoidFalseSharingSize - sizeof( std::atomic<size_type> )];

    std::atomic<size_type> m_iProduce{0}; // m_iProduceCommitted == m_iProduce -1 for pending commit.
    char m_padding3[AvoidFalseSharingSize - sizeof( std::atomic<size_type> )];

public:
    bool init( void *buf, size_type bufsize )
    {
        size_type nEntries = bufsize / sizeof( Entry );
        assert( nEntries && ( nEntries & ( nEntries - 1 ) ) == 0 ); // power 2
        nEntries = internal::next_power2_inclusive( nEntries );
        m_nEntriesMinus1 = nEntries - 1;

        auto entries = reinterpret_cast<Entry *>( buf );
        for ( size_type i = 0; i < nEntries; ++i )
            entries[i].init( i );

        m_iProduce = 0;
        m_iConsume = 0;
        //        m_iProduceCommitted = 0;
        return true;
    }

    /// \brief Pruducer pushes to the back.
    /// \return false if queue is full.
    template<class... Args>
    bool emplace( Args &&... args )
    {
        return emplace_retry( std::numeric_limits<std::size_t>::max(), std::forward<Args>( args )... );
    }

    template<class... Args>
    bool emplace_retry( const std::size_t maxtry = 1000000ul, Args &&... args )
    {
        for ( auto i = 0ul; i < maxtry; ++i )
        {
            auto iprod = m_iProduce.load( std::memory_order_acquire );
            auto &entry = m_data[iprod & m_nEntriesMinus1];
            auto iseq = entry.seq.load( std::memory_order_relaxed );

            if ( iseq == iprod ) // iseq == iprod, entry is not acquired by other producer.
            {
                if ( m_iProduce.compare_exchange_weak( iprod, iprod + 1, std::memory_order_acq_rel ) ) // try acquiring entry.
                {
                    new ( entry.getObj() ) T( std::forward<Args>( args )... );
                    entry.seq.store( iseq + 1, std::memory_order_relaxed ); // commit
                    //                    m_iProduceCommitted.store( iseq + 1, std::memory_order_release ); // finnally commit, for consumer check.
                    return true;
                }
                // else the entry is acquired by other producer.
            }
            else if ( iseq == iprod + 1 ) // entry was acquired by other producer before loading seq.
            {
                continue;
            }
            else if ( iseq + m_nEntriesMinus1 == iprod ) //  full: iseq - 1 +  CAPACITY == iprod
            {
                return false; //   assume ( std::numeric_limits<size_type>::max != m_nEntriesMinus1);
            }
            else if ( iseq == iprod + ( m_nEntriesMinus1 + 1 ) ) //  extremly unlikely, entry was consumed before loading iseq
            {
                continue;
            }
            else
            {
                assert( false && "code logic error!" );
                throw std::runtime_error( std::string( "MPMC push. Unexpected produce pos:" ) + std::to_string( iprod ) +
                                          ", iseq:" + std::to_string( iseq ) + ", icons:" + std::to_string( m_iConsume.load() ) +
                                          ", entries:" + std::to_string( m_nEntriesMinus1 + 1 ) );
            }
        }
        return false;
    }

    bool push( const T &val, const std::size_t maxtry = 1000000ul )
    {
        return emplace_retry( maxtry, val );
    }

    /// \brief Consumer pops from the front.
    bool pop( T *pObj )
    {
        size_type icons, iseq;
        Entry *pEntry;
        for ( ;; )
        {
            icons = m_iConsume.load( std::memory_order_acquire );
            pEntry = &m_data[icons & m_nEntriesMinus1];
            iseq = pEntry->seq.load( std::memory_order_relaxed );

            if ( iseq == icons + 1 )
            {
                if ( m_iConsume.compare_exchange_weak( icons, icons + 1, std::memory_order_acq_rel ) )
                    break;
            } // actually should be "else continue;"
            else if ( iseq == icons + 1 + m_nEntriesMinus1 ) // acquired by other consumer.
                continue;
            else if ( iseq == icons ) // empty
                return false;
            else if ( iseq == icons + 2 + m_nEntriesMinus1 ) // consumed by other, and produced again.
                continue;
            else
            {
                //                assert( false && "MPMC pop. program logic error!" );
                throw std::runtime_error( std::string( "MPMC pop. Unexpected consume pos:" ) + std::to_string( icons ) +
                                          ", iseq:" + std::to_string( iseq ) + ", iprod:" + std::to_string( m_iProduce.load() ) +
                                          ", entries:" + std::to_string( m_nEntriesMinus1 + 1 ) );
            }
        }

        if ( pObj )
            new ( pObj ) T( std::move( *pEntry->getObj() ) );
        pEntry->getObj()->~T();
        pEntry->seq.store( iseq + m_nEntriesMinus1, std::memory_order_relaxed );
        return true;
    }
    void clear()
    {
        while ( pop( nullptr ) )
            ;
    }
    std::size_t size() const
    {
        auto poppos = m_iConsume.load( std::memory_order_relaxed ) & m_nEntriesMinus1;
        auto pushpos = m_iProduce.load( std::memory_order_relaxed ) & m_nEntriesMinus1;
        if ( pushpos < poppos )
            return m_nEntriesMinus1 + 1 - ( poppos - pushpos );
        return pushpos - poppos;
    }
    // non-strict empty, the dequeue may be in process.
    bool empty() const
    {
        return m_iConsume.load( std::memory_order_relaxed ) == m_iProduce.load( std::memory_order_relaxed ); // weak empty
    }

    // non-strictly full, enqueue the last entry my be in process.
    bool full() const
    {
        auto iprod = m_iProduce.load( std::memory_order_acquire );
        return m_data[iprod & m_nEntriesMinus1].seq().load( std::memory_order_relaxed ) + 2 + m_nEntriesMinus1 == iprod;
    }

    /// \brief must be power of 2.
    size_type capacity() const
    {
        return m_nEntriesMinus1 + 1;
    }
};

template<class T, class AllocT, std::size_t AvoidFalseSharingSize>
class LockfreeQueue<T, true, true, AllocT, AvoidFalseSharingSize> : MPMCCircularQueueBase<T, AvoidFalseSharingSize>
{
public:
    using value_type = T;
    using base_type = MPMCCircularQueueBase<T, AvoidFalseSharingSize>;
    using base_type::clear;
    using base_type::emplace;
    using base_type::empty;
    using base_type::full;
    using base_type::init;
    using base_type::pop;
    using base_type::push;
    using base_type::size;

    constexpr static bool support_multiple_producer_threads = true, support_multiple_consumer_threads = true;
    using Alloc = typename std::allocator_traits<AllocT>::template rebind_alloc<typename base_type::Entry>;

    LockfreeQueue( size_t nCap = 0, const Alloc &alloc = Alloc() ) : m_alloc( alloc )
    {
        if ( nCap > 1 )
            init( nCap );
    }

    ~LockfreeQueue()
    {
        if ( base_type::m_data )
        {
            clear();
            m_alloc.deallocate( base_type::m_data, base_type::m_nEntriesMinus1 + 1 );
            base_type::m_data = nullptr;
        }
    }

    bool init( std::size_t nCap )
    {
        assert( nCap && ( nCap & ( nCap - 1 ) ) == 0 ); // power 2
        nCap = internal::next_power2_inclusive( nCap );
        if ( base_type::m_data )
        {
            m_alloc.deallocate( base_type::m_data, base_type::m_nEntriesMinus1 + 1 );
            base_type::m_data = nullptr;
        }
        base_type::m_data = m_alloc.allocate( nCap );
        return init( base_type::m_data, sizeof( typename base_type::Entry ) * nCap );
    }

protected:
    Alloc m_alloc;
};

template<class T, class Alloc = std::allocator<T>, std::size_t AvoidFalseSharingSize = 128>
using SPSCQueue = ftl::LockfreeQueue<T, false, false, Alloc, AvoidFalseSharingSize>;

template<class T, class Alloc = std::allocator<T>, std::size_t AvoidFalseSharingSize = 128>
using SPMCQueue = ftl::LockfreeQueue<T, false, true, Alloc, AvoidFalseSharingSize>;

template<class T, class Alloc = std::allocator<T>, std::size_t AvoidFalseSharingSize = 128>
using MPSCQueue = ftl::LockfreeQueue<T, true, false, Alloc, AvoidFalseSharingSize>;

template<class T, class Alloc = std::allocator<T>, std::size_t AvoidFalseSharingSize = 128>
using MPMCQueue = ftl::LockfreeQueue<T, true, true, Alloc, AvoidFalseSharingSize>;

} // namespace ftl
