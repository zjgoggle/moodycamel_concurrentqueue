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

////////////////////////////////////////////////////////////////////////////////////////////
///                      SPSCCircularQueue
////////////////////////////////////////////////////////////////////////////////////////////

template<class T, class AllocT = std::allocator<T>, std::size_t AvoidFalseSharingSize = 128>
class SPSCArrayQueue
{
    AllocT m_Alloc;
    size_t m_nEntriesMinus1 = 0;
    T *m_data = nullptr;
    char m_padding1[AvoidFalseSharingSize - sizeof( AllocT ) - sizeof( size_t ) - sizeof( T * )];

    std::atomic<size_t> m_iProd{0};
    size_t m_iConsForLocalProd; // consumer position used for local producer.
    char m_padding2[AvoidFalseSharingSize - sizeof( std::atomic<size_t> )];

    std::atomic<size_t> m_iCons{0};
    size_t m_iProdForLocalCons; // producer position used for local consumer.
    char m_padding3[AvoidFalseSharingSize - sizeof( std::atomic<size_t> )];

public:
    constexpr static bool support_multiple_producer_threads = false, support_multiple_consumer_threads = false;

    SPSCArrayQueue &operator=( const SPSCArrayQueue & ) = delete;

public:
    using value_type = T;

    SPSCArrayQueue( size_t cap = 0, const AllocT &alloc = AllocT() ) : m_Alloc( alloc )
    {
        if ( cap )
            init( cap );
    }

    SPSCArrayQueue( SPSCArrayQueue &&a ) : m_Alloc( a.m_Alloc ), m_nEntriesMinus1( a.m_nEntriesMinus1 ), m_data( a.m_data )
    {
        m_iProd = a.m_iProd.load();
        m_iCons = a.m_iCons.load();
        a.m_data = nullptr;
        a.m_iProd = 0;
        a.m_iCons = 0;
        if ( m_data == nullptr && m_nEntriesMinus1 > 0 )
            init( m_nEntriesMinus1 + 1 );
    }

    SPSCArrayQueue( const SPSCArrayQueue &a ) : m_Alloc( a.alloc )
    {
        init( a.m_nEntriesMinus1 + 1 );
    }

    ~SPSCArrayQueue()
    {
        destruct();
    }
    bool init( size_t cap )
    {
        assert( cap > 1 && ( cap & ( cap - 1 ) ) == 0 ); // power 2
        destruct();
        cap = internal::next_power2_inclusive( cap + 1 ); // plus 1 for the senitel in SPSC
        m_nEntriesMinus1 = cap - 1;
        m_iProd = 0;
        m_iCons = 0;
        m_iConsForLocalProd = 0;
        m_iProdForLocalCons = 0;
        if ( m_nEntriesMinus1 > 0 )
        {
            m_data = m_Alloc.allocate( cap );
            return m_data;
        }
        return true;
    }


    void destruct()
    {
        if ( m_data )
        {
            m_Alloc.deallocate( m_data, m_nEntriesMinus1 + 1 );
            m_data = nullptr;
        }
    }

    size_t capacity() const
    {
        return m_nEntriesMinus1; //
    }
    bool full() const
    {
        return m_nEntriesMinus1 == 0 ||
               ( ( m_iProd.load( std::memory_order_relaxed ) + 1 ) & m_nEntriesMinus1 ) == m_iCons.load( std::memory_order_relaxed );
    }
    bool empty() const
    {
        return m_nEntriesMinus1 == 0 || m_iProd.load( std::memory_order_relaxed ) == m_iCons.load( std::memory_order_relaxed );
    }
    size_t size() const
    {
        auto pushpos = m_iProd.load( std::memory_order_relaxed ) & m_nEntriesMinus1;
        auto poppos = m_iCons.load( std::memory_order_relaxed ) & m_nEntriesMinus1;
        return pushpos >= poppos ? ( pushpos - poppos ) : ( m_nEntriesMinus1 + 1 - poppos + pushpos );
    }

    // get the position for push operation.
    T *prepare_push() const
    {
        if ( !m_nEntriesMinus1 )
            return nullptr;
        auto pushpos = m_iProd.load( std::memory_order_relaxed ) & m_nEntriesMinus1;
        auto poppos = m_iCons.load( std::memory_order_relaxed ) & m_nEntriesMinus1;
        if ( ( ( pushpos + 1 ) & m_nEntriesMinus1 ) == poppos ) // full
            return nullptr;
        return m_data + pushpos;
    }
    // prepare_push must be called before commit_push
    void commit_push()
    {
        m_iProd.fetch_add( 1, std::memory_order_acq_rel );
    }

    template<class... Args>
    T *emplace( Args &&... args )
    {
        assert( m_nEntriesMinus1 );
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

    bool enqueue( const T &val )
    {
        assert( m_nEntriesMinus1 );

        auto iProd = m_iProd.load( std::memory_order_relaxed ) & m_nEntriesMinus1;
        if ( ( iProd + 1 ) == m_iConsForLocalProd ) // possibly full
        {
            m_iConsForLocalProd = m_iCons.load( std::memory_order_relaxed ) & m_nEntriesMinus1;
            if ( ( iProd + 1 ) == m_iConsForLocalProd ) // full
                return false;
        }
        new ( &m_data[iProd] ) T( std::move( val ) );
        m_iProd.store( iProd + 1, std::memory_order_release );
        return true;
    }

    bool try_dequeue( T &val )
    {
        assert( m_nEntriesMinus1 );

        auto iCons = m_iCons.load( std::memory_order_relaxed ) & m_nEntriesMinus1;
        if ( m_iProdForLocalCons == iCons ) // prossibly empty
        {
            m_iProdForLocalCons = m_iProd.load( std::memory_order_relaxed ) & m_nEntriesMinus1;
            if ( iCons == m_iConsForLocalProd ) // empty
                return false;
        }
        val = std::move( m_data[iCons] );
        m_data[iCons].~T();
        m_iCons.store( iCons + 1, std::memory_order_release );
        return true;
    }

    T *top() const
    {
        if ( !m_nEntriesMinus1 )
            return nullptr;
        auto pushpos = m_iProd.load( std::memory_order_relaxed ) & m_nEntriesMinus1;
        auto poppos = m_iCons.load( std::memory_order_relaxed ) & m_nEntriesMinus1;
        if ( pushpos == poppos ) // empty
            return nullptr;
        return &m_data[poppos];
    }
    // v: uninitialized memory
    bool pop( T *buf = nullptr )
    {
        if ( !m_nEntriesMinus1 )
            return false;
        auto iProd = m_iProd.load( std::memory_order_relaxed ) & m_nEntriesMinus1;
        auto iCons = m_iCons.load( std::memory_order_relaxed ) & m_nEntriesMinus1;
        if ( iProd == iCons ) // empty
            return false;
        if ( buf )
            new ( buf ) T( std::move( m_data[iCons] ) );
        m_data[iCons].~T();
        m_iCons.store( iCons + 1, std::memory_order_release );
        return true;
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

    MPMCCircularQueueBase( const MPMCCircularQueueBase & ) = delete;
    MPMCCircularQueueBase &operator=( const MPMCCircularQueueBase & ) = delete;

public:
    MPMCCircularQueueBase() = default;
    MPMCCircularQueueBase( MPMCCircularQueueBase &&a )
        : m_nEntriesMinus1( a.m_nEntriesMinus1 ), m_data( a.m_data ), m_iConsume( a.m_iConsume.load() ), m_iProduce( a.m_iProduce.load() )
    {
        a.m_data = nullptr;
    }
    MPMCCircularQueueBase &operator=( MPMCCircularQueueBase &&a )
    {
        m_nEntriesMinus1 = a.m_nEntriesMinus1;
        m_data = a.m_nEntriesMinus1;
        m_iConsume = a.m_iConsume.load();
        m_iProduce = a.m_iProduce.load();
        return *this;
    }

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

    // single producer version
    template<class... Args>
    bool emplace_sp( Args &&... args )
    {
        auto iprod = m_iProduce.load( std::memory_order_relaxed );
        auto &entry = m_data[iprod & m_nEntriesMinus1];
        auto seq = entry.seq.load( std::memory_order_acquire );

        if ( seq == iprod )
        {
            m_iProduce.store( iprod + 1, std::memory_order_relaxed );

            new ( entry.getObj() ) T( std::forward<Args>( args )... );
            entry.seq.store( seq + 1, std::memory_order_release ); // inc seq. when popping, seq == poppos+1
            return true;
        }
        else
        {
            assert( seq + m_nEntriesMinus1 == iprod && "program logic error!" ); // full
            return false;
            //            throw std::runtime_error( std::string( "MPSC push. Unexpected produce pos:" ) + std::to_string( iprod ) +
            //                                      ", iseq:" + std::to_string( seq ) + ", entries:" + std::to_string( m_nEntriesMinus1 + 1 ) );
        }
    }

    bool push( const T &val, const std::size_t maxtry = 1000000ul )
    {
        return emplace_retry( maxtry, val );
    }
    bool enqueue( const T &val, const std::size_t maxtry = 1000000ul )
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
    bool try_dequeue( T &obj )
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

        obj = std::move( *pEntry->getObj() );
        pEntry->getObj()->~T();
        pEntry->seq.store( iseq + m_nEntriesMinus1, std::memory_order_relaxed );
        return true;
    }
    bool try_dequeue_sc( T &val )
    {
        auto icons = m_iConsume.load( std::memory_order_acquire );
        auto &entry = m_data[icons & m_nEntriesMinus1];
        auto seq = entry.seq.load( std::memory_order_acquire );

        if ( icons + 1 == seq )
        {
            m_iConsume.store( icons + 1, std::memory_order_relaxed );
            val = std::move( *entry.getObj() );
            entry.getObj()->~T();
            entry.seq.store( seq + m_nEntriesMinus1, std::memory_order_release ); // dec seq and plus mCap. when next push, seq == pushpos.
            return true;
        }
        else
        { // empty
            assert( icons == seq && "program logic error!" );
            //            throw std::runtime_error( std::string( "MPSC pop. Unexpected consume pos:" ) + std::to_string( icons ) +
            //                                      ", iseq:" + std::to_string( seq ) + ", iProd:" + std::to_string( m_iProduce.load() ) +
            //                                      ", entries:" + std::to_string( m_nEntriesMinus1 + 1 ) );
            return false;
        }
    }
    bool pop_sc( T *pObj = nullptr )
    {
        auto icons = m_iConsume.load( std::memory_order_acquire );
        auto &entry = m_data[icons & m_nEntriesMinus1];
        auto seq = entry.seq.load( std::memory_order_acquire );

        if ( icons + 1 == seq )
        {
            m_iConsume.store( icons + 1, std::memory_order_relaxed );
            new ( pObj ) T( std::move( *entry.getObj() ) );
            entry.getObj()->~T();
            entry.seq.store( seq + m_nEntriesMinus1, std::memory_order_release ); // dec seq and plus mCap. when next push, seq == pushpos.
            return true;
        }
        else
        { // empty
            assert( icons == seq && "program logic error!" );
            //            throw std::runtime_error( std::string( "MPSC pop. Unexpected consume pos:" ) + std::to_string( icons ) +
            //                                      ", iseq:" + std::to_string( seq ) + ", iProd:" + std::to_string( m_iProduce.load() ) +
            //                                      ", entries:" + std::to_string( m_nEntriesMinus1 + 1 ) );
            return false;
        }
    }
    // only for Single Consumer
    T *top()
    {
        auto icons = m_iConsume.load( std::memory_order_acquire );
        auto &entry = m_data[icons & m_nEntriesMinus1];
        auto seq = entry.seq.load( std::memory_order_acquire );

        return icons + 1 == seq ? entry.getObj() : nullptr;
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


template<class T, bool bMultiProcuer, bool MultiConsumer, class AllocT = std::allocator<T>, std::size_t AvoidFalseSharingSize = 128>
class LockfreeQueue;

template<class T, class AllocT, std::size_t AvoidFalseSharingSize>
class LockfreeQueue<T, true, true, AllocT, AvoidFalseSharingSize> : public MPMCCircularQueueBase<T, AvoidFalseSharingSize>
{
public:
    using value_type = T;
    using base_type = MPMCCircularQueueBase<T, AvoidFalseSharingSize>;
    using base_type::clear;
    using base_type::emplace;
    using base_type::emplace_sp;
    using base_type::empty;
    using base_type::enqueue;
    using base_type::full;
    using base_type::init;
    using base_type::pop;
    using base_type::push;
    using base_type::size;
    using base_type::top;
    using base_type::try_dequeue_sc;

    constexpr static bool support_multiple_producer_threads = true, support_multiple_consumer_threads = true;
    using Alloc = typename std::allocator_traits<AllocT>::template rebind_alloc<typename base_type::Entry>;

    LockfreeQueue( size_t nCap = 0, const Alloc &alloc = Alloc() ) : m_alloc( alloc )
    {
        if ( nCap > 1 )
            init( nCap );
    }
    LockfreeQueue( LockfreeQueue &&a ) : base_type( std::move( a ) ), m_alloc( std::move( a.m_alloc ) )
    {
        if ( !base_type::m_data && a.base_type::m_nEntriesMinus1 )
            init( base_type::m_nEntriesMinus1 + 1 );
    }
    LockfreeQueue &operator=( LockfreeQueue &&a )
    {
        this->~LockfreeQueue();
        new ( this ) LockfreeQueue( std::move( a ) );
        return *this;
    }

    ~LockfreeQueue()
    {
        if ( base_type::m_data )
        {
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

// SPSC
template<class T, class AllocT, std::size_t AvoidFalseSharingSize>
class LockfreeQueue<T, false, false, AllocT, AvoidFalseSharingSize> : public LockfreeQueue<T, true, true, AllocT, AvoidFalseSharingSize>
{
public:
    using value_type = T;
    using base_type = LockfreeQueue<T, true, true, AllocT, AvoidFalseSharingSize>;
    using base_type::capacity;
    using base_type::empty;
    using base_type::enqueue;
    using base_type::init;
    using base_type::push;
    using base_type::size;
    using base_type::top;


    LockfreeQueue( std::size_t cap = 0, typename base_type::Alloc alloc = {} ) : base_type( cap, alloc )
    {
    }

    bool try_dequeue( T &val )
    {
        return base_type::try_dequeue_sc( val );
    }
    bool enqueue( const T &val )
    {
        return base_type::emplace_sp( val );
    }
    bool pop( T *pObj = nullptr )
    {
        return base_type::pop_sc( pObj );
    }
};

// MPSC
template<class T, class AllocT, std::size_t AvoidFalseSharingSize>
class LockfreeQueue<T, true, false, AllocT, AvoidFalseSharingSize> : LockfreeQueue<T, true, true, AllocT, AvoidFalseSharingSize>
{
public:
    using value_type = T;
    using base_type = LockfreeQueue<T, true, true, AllocT, AvoidFalseSharingSize>;
    using base_type::capacity;
    using base_type::empty;
    using base_type::enqueue;
    using base_type::init;
    using base_type::push;
    using base_type::size;
    using base_type::top;

    LockfreeQueue( std::size_t cap = 0, typename base_type::Alloc alloc = {} ) : base_type( cap, alloc )
    {
    }

    bool try_dequeue( T &val )
    {
        return base_type::try_dequeue_sc( val );
    }
    bool pop( T *pObj = nullptr )
    {
        return base_type::pop_sc( pObj );
    }
};

// SPMC
template<class T, class AllocT, std::size_t AvoidFalseSharingSize>
class LockfreeQueue<T, false, true, AllocT, AvoidFalseSharingSize> : LockfreeQueue<T, true, true, AllocT, AvoidFalseSharingSize>
{
public:
    using value_type = T;
    using base_type = LockfreeQueue<T, true, true, AllocT, AvoidFalseSharingSize>;
    using base_type::capacity;
    using base_type::empty;
    using base_type::init;
    using base_type::pop;
    using base_type::push;
    using base_type::size;
    using base_type::try_dequeue;

    LockfreeQueue( std::size_t cap = 0, typename base_type::Alloc alloc = {} ) : base_type( cap, alloc )
    {
    }


    bool enqueue( const T &val )
    {
        return base_type::emplace_sp( val );
    }
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
