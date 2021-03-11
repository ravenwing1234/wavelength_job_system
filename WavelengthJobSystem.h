/**
* WavelengthJobSystem.h
* A Lambda function based job system
* Requires C++17
* Created by: Andrew Nguyen
* https://github.com/ravenwing1234/wavelength_job_system
* 
* By default, has dependency on a MPMC queue. Replace "using MPMCQueue" if needed.
* Otherwise by default, this job system uses a concurrent queue from: https://github.com/cameron314/concurrentqueue
* 
* 
*   Usage:
* 
*       // Init job system before dispatching any jobs:
* 
*           wave::InitGlobalJobSystem();
* 
*       // To shutdown job system:
* 
*           wave::ShutdownGlobalJobSystem();
* 
*       // To dispatch an async job:
* 
*           wave::job::AsyncJob( []() 
*               {
*                   // Do job
*               } );
* 
*           **note**: For optimal performance, limit the lambda capture payload to 48 bytes of data.
*                     Modify the int literal of SBO_SIZE to a pointer-sized interval of bytes to increase. Ideally cache-line sized.
*       
*       // To dispatch an async job with a callback:
* 
*           wave::job::AsyncJobWithCallback( [](){ // Job }, [](){ // Callback } );
* 
*       // To dispatch an async job with a return value as a future that is promised to be filled:
* 
*           auto futureResult = wave::job::AsyncJobWithFuture( []()->int { // Job w/ return int; } );
*           int result = futureResult.get();
* 
*       // You can create a job that will automatically sync upon closing scope
*       // Scope the synchronization with { }. Exiting scope waits for task complete
*       // Will block calling thread from leaving scope till job is completed
*           
*          { wave::job::SyncronousJob syncJobObj( [...](...){...} ); 
*               // Do other work here
*          } // Will wait here for syncJobObj to be finished
*            
*       // To flush all pending callback functions:
* 
*           wave::job::FlushPendingJobCallbacks(); 
* 
*       // Wait for future to be filled
* 
*           wave::job::WaitForFuture( *FutureVar* );
* 
*       // Have the current thread assist with processing jobs until a condition is met
* 
*           ProcessJobsUntil( ConditionIsTrue );
* 
*   BSD 2-Clause License
*   
*   Copyright (c) 2021, Andrew Nguyen
*   All rights reserved.
*   
*   Redistribution and use in source and binary forms, with or without
*   modification, are permitted provided that the following conditions are met:
*   
*   1. Redistributions of source code must retain the above copyright notice, this
*      list of conditions and the following disclaimer.
*   
*   2. Redistributions in binary form must reproduce the above copyright notice,
*      this list of conditions and the following disclaimer in the documentation
*      and/or other materials provided with the distribution.
*   
*   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
*   AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
*   IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
*   DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
*   FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
*   DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
*   SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
*   CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
*   OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
*   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.   
*   
*/

#pragma once

#include "concurrentqueue.h"
#include <array>
#include <cstdint>
#include <atomic>
#include <thread>
#include <vector>
#include <future>
#include <unordered_map>
#include <type_traits>
#include <chrono>

namespace wave
{

template< typename T >
using MPMCQueue = moodycamel::ConcurrentQueue< T >; // <------Specify your multi-producer multi-consumer queue here

using f64 = double;
using JobCounter = std::atomic< int >;
constexpr int SBO_SIZE = 64 - sizeof(void*);

/**
* Clock
* Interface to query highest precision timing on platform
*/

class Clock
{
public:

    static inline f64 GetAbsoluteTimeSeconds()
    {
        return std::chrono::duration_cast<std::chrono::duration< f64 >>(std::chrono::high_resolution_clock::now().time_since_epoch()).count();
    }

    static inline f64 ApplicationTimeSeconds() // Seconds since application started running
    {
		static f64 s_applicationBeginTime = GetAbsoluteTimeSeconds();
		return GetAbsoluteTimeSeconds() - s_applicationBeginTime;
    }

};

/**
* ThreadRAII
* Request Acquisition Is Initialization pattern wrapper for std::thread
*
* Reference: "Effective Modern C++" by: Scott Meyers
*/
class ThreadRAII
{
public:

	enum class DtorAction { JOIN, DETACH };

	ThreadRAII( std::thread&& _thread, DtorAction dtorAction = DtorAction::JOIN )
		: m_dtorAction( dtorAction )
		, m_thread( std::move( _thread ) ) 
	{}

	ThreadRAII( const ThreadRAII& )				= delete;
	ThreadRAII& operator=( const ThreadRAII& )	= delete;

	ThreadRAII( ThreadRAII&& other ) noexcept
	{
		*this = std::move( other );
	}
	
	ThreadRAII& operator=( ThreadRAII&& other ) noexcept
	{
		m_dtorAction = std::move( other.m_dtorAction );
		m_thread = std::move( other.m_thread );
		return *this;
	}

	~ThreadRAII()
	{
		if( m_thread.joinable() )
		{
			if( m_dtorAction == DtorAction::JOIN )
			{
				m_thread.join();
			}
			else
			{
				m_thread.detach();
			}
		}
	}

	std::thread& Get() { return m_thread; }

private:

	DtorAction	m_dtorAction;
	std::thread m_thread;

};

/**
* JobEnums
*
*/

namespace EJobPriority
{
enum EPriority
{
	BLOCKING = 0,
	LOW,
	NORMAL,
	HIGH,

	NUM_PRIORITIES
};
}

/**
* Job
* Object type that represents a job to be processed
*
*/

//////////////////////////////////////////////////////////////////////////
/// Job interface for processing by job system for Type-Erasure
//////////////////////////////////////////////////////////////////////////
class IBaseJob
{
public:

	virtual void ExecuteJob() = 0;
    virtual ~IBaseJob() {}
};

//////////////////////////////////////////////////////////////////////////
/// Job that executes a passed in functor of template type
//////////////////////////////////////////////////////////////////////////
template< typename FunctionType >
class FunctorJob : public IBaseJob
{
public:

	explicit FunctorJob( FunctionType&& func )
		: m_func( std::move( func ) )
	{}

    virtual ~FunctorJob() {}

	virtual void ExecuteJob() override
	{
		m_func();
	}

protected:

	FunctionType m_func;
};

#pragma warning( push )
#pragma warning( disable:4127 )

class Job
{
public:

	Job() 
	{}

	~Job()
	{}

	template< typename FunctionType >
	Job( FunctionType&& func )
	{
		SetFunction( std::move( func ) );
	}

	Job( Job&& other ) noexcept
		: m_localBuffer( other.m_localBuffer )
		, m_jobPtr( reinterpret_cast< IBaseJob* >( &m_localBuffer[ 0 ] ) )
	{
		if( !isUsingBuffer() )
		{
			m_jobPtr = std::move( other.m_jobPtr );
		}
	}

	Job( const Job& other ) = delete;

	void operator=( Job&& other ) noexcept
	{
		m_localBuffer = other.m_localBuffer;
		if( !isUsingBuffer() )
		{
			m_jobPtr = other.m_jobPtr;
		}
		else
		{
			m_jobPtr = reinterpret_cast< IBaseJob* >( &m_localBuffer[ 0 ] );
		}
	}
	
	void operator=( const Job& other ) = delete;

	bool operator() ()
	{
		if( m_jobPtr )
		{
			m_jobPtr->ExecuteJob();
			if( !isUsingBuffer() )
			{
				delete m_jobPtr;
			}
			return true;
		}
		return false;
	}

	template< typename FunctionType >
	inline void SetFunction( FunctionType&& func )
	{
		constexpr int maxFuncSize = SBO_SIZE - sizeof( void* );
		if( sizeof( func ) <= maxFuncSize )
		{		
			m_jobPtr = new( m_localBuffer.data() ) FunctorJob< FunctionType >( std::move( func ) );	
		}
		else
		{
			m_jobPtr = new FunctorJob< FunctionType >( std::move( func ) );
		}
	}

private:

	inline bool isUsingBuffer()
	{
		return *reinterpret_cast< std::uint64_t* >( m_localBuffer.data() ) != NULL;
	}

public:

	IBaseJob* m_jobPtr = { nullptr };

private:

	std::array< char, SBO_SIZE > m_localBuffer = {};
};

#pragma warning( pop )

/**
* JobManager
* Manages job worker threads that drain a work queue
* See Job for functions that utilize global job system.
* Access namespace global::job:: to push global jobs.
*
* Must explicitly be initalized and shutdown with Init() after constructions
* and Shutdown() before destruction.
* See Job for wrapper functions to dispatch jobs to global job manager
*/

/// Type definitions
using SectionLockRecursive = std::recursive_mutex;
using SectionLock = std::mutex;
using SectionLockGuard = std::lock_guard < SectionLock >;
using SectionLockGuardRecursive = std::lock_guard< SectionLockRecursive >;
///

constexpr int MAX_CALLBACKS_PER_FRAME = 128;
constexpr int MAX_WORKER_THREADS = 32;

class JobManager
{
public:

	JobManager() {}
	JobManager( int numWorkers, bool bCreateBlockingWorker = false )
    : m_maxWorkerCount( numWorkers )
	, m_bBlockingThreadActive( bCreateBlockingWorker )
    {}

	~JobManager() {}
	JobManager( const JobManager& )				= delete;
	JobManager( JobManager&& )					= delete;
	JobManager& operator=( const JobManager& )	= delete;
	JobManager& operator=( JobManager&& )		= delete;

	inline void Init()
    {
        init( m_bBlockingThreadActive );
    }
	
    inline void ShutDown()
    {
        SectionLockGuardRecursive threadLock( m_threadManagementLock );
        m_bBlockingThreadActive = false;
        SetMaxThreadCount( 0 );
        if( !m_workerThreads.empty() )
        {
            m_workerThreads.pop_back();
        }
        SectionLockGuard callbackLock( m_callbackLock );
    }

	//////////////////////////////////////////////////////////////////////////

    /**
    * Queue a functor task to be processed by job system
    * Performs a move on the passed in functor and places in the appropriate priority queue
    */
	template< typename FunctionType >
	inline void QueueJob( FunctionType&& func, EJobPriority::EPriority priority = EJobPriority::NORMAL )
    {
        m_workQueue[ priority ].enqueue( Job( std::move( func ) ) );
        notifyWaitingWorkers( priority );
    }

    /**
    * Queue a functor task to be processed by job system
    * Additionally queues a function callback to be fired on FlushJobCallbacks in calling thread
    */
	template< typename FunctionType, typename CallbackType >
	inline void QueueJobWithCallback( FunctionType&& func, CallbackType&& callback, EJobPriority::EPriority priority = EJobPriority::NORMAL )
    {
        std::thread::id callerThreadId = std::this_thread::get_id();
        auto asyncJobWithCallback = std::bind(
            [ this, callerThreadId ]( FunctionType _func, CallbackType _callback )
            {
                _func();
                m_finishedJobCallbacks[ callerThreadId ].enqueue( Job( std::move( _callback ) ) );
            },
            std::move( func ),
            std::move( callback )
        );
        QueueJob( std::move( asyncJobWithCallback ), priority );
    }

    /**
    * Queue a functor task with a return value to be processed by job system
    * Return value of passed in functor is returned to caller as a std::future
    */
	template< typename FunctionType >
	inline std::future< typename std::invoke_result< FunctionType >::type > QueueJobWithFuture( FunctionType&& func, EJobPriority::EPriority priority = EJobPriority::NORMAL )
    {
        std::packaged_task< std::invoke_result< FunctionType >::type() > task( std::move( func ) );
        std::future< std::invoke_result< FunctionType >::type > result( task.get_future() );
        m_workQueue[ priority ].enqueue( Job( std::move( task ) ) );
        notifyWaitingWorkers( priority );

        return result;
    }
    
	//////////////////////////////////////////////////////////////////////////

    /**
    * Calling thread waiting for future can help process jobs
    */
	template< typename FutureType >
	inline void WaitForFuture( std::future< FutureType >& futureValue )
    {
        while( futureValue.wait_for( std::chrono::seconds( 0 ) ) == std::future_status::timeout )
        {
            ProcessNextJob();
        }
    }

	inline bool ProcessNextJob()
    {
        Job currentJob;
        for( int currentPriority = EJobPriority::HIGH; currentPriority >= EJobPriority::LOW; --currentPriority )
        {
            if( m_workQueue[ currentPriority ].try_dequeue( currentJob ) )
            {
                break;
            }
        }
        return currentJob();
    }

	inline void FlushJobCallbacks()
    {
        SectionLockGuard lock( m_callbackLock );
        Job callbacks[ MAX_CALLBACKS_PER_FRAME ];
        const std::thread::id callerThreadId = std::this_thread::get_id();
        const std::size_t callbackCount = m_finishedJobCallbacks[ callerThreadId ].try_dequeue_bulk( callbacks, MAX_CALLBACKS_PER_FRAME );
        for( std::size_t i = 0; i < callbackCount; ++i )
        {
            callbacks[i]();
        }
    }

	inline void SetMaxThreadCount( int maxWorkerCount)
    {
        {SectionLockGuardRecursive lock( m_threadManagementLock );
            m_maxWorkerCount = maxWorkerCount;
        }
        m_workerWaitCondition.notify_all();
        m_blockingWorkerWaitCondition.notify_all();
        updateThreadPool();
    }

	inline int GetMaxThreadCount() const { return m_maxWorkerCount; }
	inline int GetCurrentThreadCount() const { return m_currentWorkerCount; }

private:

	inline void init( bool bCreateBlockingWorker )
    {
        SectionLockGuardRecursive lock(m_threadManagementLock);

        // Create blocking thread at index 0
        if( bCreateBlockingWorker )
        {
            m_workerThreads.emplace_back( std::thread( &JobManager::workerBlockingThreadMain, this ) );
        }

        // Create general worker threads
        for( m_currentWorkerCount; m_currentWorkerCount < m_maxWorkerCount; ++m_currentWorkerCount )
        {
            m_workerThreads.emplace_back( std::thread( &JobManager::workerGenericThreadMain, this ) );
        }
    }

	inline void updateThreadPool()
    {
        SectionLockGuardRecursive lock( m_threadManagementLock );
        for( m_currentWorkerCount; m_currentWorkerCount < m_maxWorkerCount; ++m_currentWorkerCount )
        {
            m_workerThreads.emplace_back( std::thread( &JobManager::workerGenericThreadMain, this ) );
        }
        for( m_currentWorkerCount; m_currentWorkerCount > m_maxWorkerCount; --m_currentWorkerCount )
        {
            m_workerThreads.pop_back();
        }
    }

	inline void workerGenericThreadMain()
    {
        m_workerThreadLock.lock();
        static int s_nextWorkerID = 0;
        const int workerID = s_nextWorkerID;

        ++s_nextWorkerID;
        m_workerThreadLock.unlock();
        
        constexpr f64 maxTimeWithNoJob = 0.003;
        f64 lastTime = Clock::GetAbsoluteTimeSeconds();
        f64 timeSinceLastJob = 0.0;

        while( workerID < m_maxWorkerCount )
        {
            if( !ProcessNextJob() )
            {
                f64 currTime = Clock::GetAbsoluteTimeSeconds();
                f64 deltaTime = currTime - lastTime;
                timeSinceLastJob += deltaTime;
                lastTime = currTime;
                if( timeSinceLastJob > maxTimeWithNoJob )
                {
                    std::unique_lock< SectionLock > waitLock( m_signalLock );
                    m_waitingThreadFlags |= 1 << workerID;
                    m_workerWaitCondition.wait( waitLock );
                    m_waitingThreadFlags &= ~( 1 << workerID );
                }
                else
                {
                    std::this_thread::yield();
                }
            }
            else
            {
                timeSinceLastJob = 0;
            }
        }

        m_workerThreadLock.lock();
            --s_nextWorkerID;
        m_workerThreadLock.unlock();
    }

	inline void workerBlockingThreadMain()
    {
        constexpr int maxBulkCount = 64;
        constexpr f64 maxTimeWithNoJob = 0.003;
        f64 lastTime = Clock::GetAbsoluteTimeSeconds();
        f64 timeSinceLastJob = 0.0;

        while( m_bBlockingThreadActive )
        {
            Job currentJob[ maxBulkCount ];
            const std::size_t numJobs = m_workQueue[ EJobPriority::BLOCKING ].try_dequeue_bulk( currentJob, maxBulkCount );
            if( numJobs )
            {
                for( std::size_t i = 0; i < numJobs; ++i )
                {
                    currentJob[i]();
                }
            }
            else
            {
                f64 currTime = Clock::GetAbsoluteTimeSeconds();
                f64 deltaTime = currTime - lastTime;
                timeSinceLastJob += deltaTime;
                lastTime = currTime;
                if( timeSinceLastJob > maxTimeWithNoJob )
                {
                    std::unique_lock< SectionLock > waitLock( m_blockingThreadSignalLock );
                    m_bBlockingThreadWaiting = true;
                    m_blockingWorkerWaitCondition.wait( waitLock );
                    m_bBlockingThreadWaiting = false;
                }
                else
                {
                    std::this_thread::yield();
                }
            }
        }
    }

	inline void notifyWaitingWorkers( EJobPriority::EPriority priority )
    {
        switch( priority )
        {
            case wave::EJobPriority::BLOCKING:
            {
                if( m_bBlockingThreadWaiting )
                {
                    m_blockingWorkerWaitCondition.notify_all();
                }
                break;
            }
            default:
            {
                if( m_waitingThreadFlags > 0 )
                {
                    m_workerWaitCondition.notify_one();
                }
                break;
            }
        }
    }

private:

	std::vector< ThreadRAII >													m_workerThreads;
	MPMCQueue< Job >															m_workQueue[EJobPriority::NUM_PRIORITIES];
	std::unordered_map< std::thread::id, MPMCQueue< Job > >						m_finishedJobCallbacks;
	SectionLockRecursive														m_threadManagementLock;
	SectionLock																	m_callbackLock;
	SectionLock																	m_workerThreadLock;
	SectionLock																	m_signalLock;
	SectionLock																	m_blockingThreadSignalLock;
	std::condition_variable														m_workerWaitCondition;
	std::condition_variable														m_blockingWorkerWaitCondition;
	volatile int																m_waitingThreadFlags = { 0 };
	int																			m_currentWorkerCount = { 0 };
	volatile int																m_maxWorkerCount = { static_cast<int>(std::thread::hardware_concurrency() < MAX_WORKER_THREADS ? std::thread::hardware_concurrency() : MAX_WORKER_THREADS) };
	volatile bool																m_bBlockingThreadActive = { true };
	volatile bool																m_bBlockingThreadWaiting = { false };
};

/**
* JobSystem
* Global job system and job helper functions
*
*/

/// Global job manager subsystem
inline std::unique_ptr< JobManager > gJobManager;

inline void InitGlobalJobSystem()
{
    gJobManager = std::make_unique< JobManager >();
	gJobManager->Init();
}

inline void ShutdownGlobalJobSystem()
{
	gJobManager->ShutDown();
	gJobManager = nullptr;
}

#define ProcessJobsUntil( condition ) \
if( wave::gJobManager ) \
{ \
	while( !( condition ) ) \
	{ \
		wave::gJobManager->ProcessNextJob(); \
	} \
}

namespace job
{

/// Global Job system flush
inline void FlushPendingJobCallbacks()
{
	gJobManager->FlushJobCallbacks();
}

/// Global wait for future task helper
template< typename FutureType >
void WaitForFuture( std::future< FutureType >& futureValue )
{
	gJobManager->WaitForFuture( futureValue );
}

//////////////////////////////////////////////////////////////////////////
/// Pushes a job functor on job system with a callback on completion
/// Callback fires on caller thread
//////////////////////////////////////////////////////////////////////////
template< typename FunctionType, typename CallbackType >
inline void AsyncJobWithCallback( FunctionType&& func, CallbackType&& callback, EJobPriority::EPriority priority = EJobPriority::NORMAL )
{
	if( gJobManager )
	{
		gJobManager->QueueJobWithCallback( std::move( func ), std::move( callback ), priority );
	}
	else
	{
		func();
		callback();
	}
}

//////////////////////////////////////////////////////////////////////////
/// Push an async job functor on job system
/// Usage: ex. AsyncronousJob( [...](...){...} );
//////////////////////////////////////////////////////////////////////////
template< typename FunctionType >
inline void AsyncJob( FunctionType&& func, EJobPriority::EPriority priority = EJobPriority::NORMAL )
{
	if( gJobManager )
	{
		gJobManager->QueueJob( std::move( func ), priority );
	}
	else
	{
		func();
	}
}

//////////////////////////////////////////////////////////////////////////
/// Pushes an async job functor on job system
/// Returns the functor return value as a std::future
//////////////////////////////////////////////////////////////////////////
template< typename FunctionType >
inline std::future< typename std::invoke_result< FunctionType >::type > AsyncJobWithFuture( FunctionType&& func, EJobPriority::EPriority priority = EJobPriority::NORMAL )
{
	return std::move( gJobManager->QueueJobWithFuture( std::move( func ), priority ) );
}

//////////////////////////////////////////////////////////////////////////
/// Wrapper for pushing a synchronous job functor.
/// Usage: ex. { SyncronousJob syncJobObj( [&...](...){...} ); }
/// Scope the synchronization with { }. Exiting scope waits for task complete
/// Will block calling thread from leaving scope till job is completed
//////////////////////////////////////////////////////////////////////////
class SynchronousJob
{
public:

	template< typename FunctionType >
	explicit SynchronousJob( FunctionType&& func, EJobPriority::EPriority priority = EJobPriority::NORMAL )
	{
		auto syncJob = std::bind(
			[this]( FunctionType _func )
			{
				_func();
				m_promise.set_value();
			},
		std::move( func )
		);
		gJobManager->QueueJob( std::move( syncJob ), priority );
	}

	~SynchronousJob()
	{
		m_promise.get_future().wait();
	}

	SynchronousJob() = delete;
	SynchronousJob( const SynchronousJob& ) = delete;
	SynchronousJob( SynchronousJob&& ) = delete;
	SynchronousJob& operator=( const SynchronousJob& ) = delete;
	SynchronousJob& operator=( SynchronousJob&& ) = delete;

private:

	std::promise< void > m_promise;
};

}

}
