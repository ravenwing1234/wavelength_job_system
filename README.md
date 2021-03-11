# Wavelength Job System 
Lambda function based Modern C++ Job System

## Dependendies:
- C++17
- A MPMC Queue. Currently using an amazing implemention from https://github.com/cameron314/concurrentqueue

## How to setup:
1. Copy WavelengthJobSystem.h to your project.
2. Copy concurrentqueue.h OR specify your own multi-producer multi-consumer queue by replacing the alias 

	template< typename T >
	using MPMCQueue = (*Your multi-producer multi-consumer queue*);

## Basic Usage:
### Init job system before dispatching any jobs: 

    wave::InitGlobalJobSystem();
 
### To shutdown job system:
 
	wave::ShutdownGlobalJobSystem();
 
### To dispatch an async job:
 
    wave::job::AsyncJob( []() 
    {
        // Do job
    } );
 
**note**: For optimal performance, limit the lambda capture payload to 48 bytes of data.
          Modify the int literal of SBO_SIZE to a pointer-sized interval of bytes to increase.
		  Ideally cached-line sized.

### To dispatch an async job with a callback:
 
	wave::job::AsyncJobWithCallback( 
		[](){ // Job }, 
		[](){ // Callback } 
	);
 
### To dispatch an async job with a return value as a future that is promised to be filled:
 
	auto futureResult = wave::job::AsyncJobWithFuture( 
		[]()->int 
		{ 
			// Job w/ return int; 
		} );  
	int result = futureResult.get();
	
### You can create a job that will automatically sync upon closing scope
Scope the synchronization with { }. Exiting scope waits for task complete
Will block calling thread from leaving scope till job is completed
    
	{ wave::job::SyncronousJob syncJobObj( [...](...)
		{
			...
		} ); 
	    
		// Do other work here
	} // Will wait here for syncJobObj to be finished
	 
### To flush all pending callback functions:
	 
	wave::job::FlushPendingJobCallbacks(); 

### Wait for future to be filled

    wave::job::WaitForFuture( FutureVariable );

### Have the current thread assist with processing jobs until a condition is met

    ProcessJobsUntil( ConditionIsTrue );