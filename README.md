# Wavelength Job System 
Lambda function based Modern C++ Job System

## Dependendies:
C++17
A MPMC Queue. Currently using an amazing implemention from https://github.com/cameron314/concurrentqueue

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

### To dispatch an async job with a callback:
 
	wave::job::AsyncJobWithCallback( [](){ // Job }, [](){ // Callback } );
 
### To dispatch an async job with a return value as a future that is promised to be filled:
 
	auto futureResult = wave::job::AsyncJobWithFuture( []()->int { // Job w/ return int; } );  
	int result = futureResult.get();
 
### To flush all pending callback functions:
 
	void FlushPendingJobCallbacks(); 
