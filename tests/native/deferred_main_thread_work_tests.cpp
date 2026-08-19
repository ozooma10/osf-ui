#include "Runtime/DeferredMainThreadWork.h"

#include <cassert>
#include <iostream>

int main()
{
	OSFUI::DeferredMainThreadWork work;
	assert(!work.Take());

	work.Request();
	assert(work.Take());
	assert(!work.Take());

	// Multiple callback-thread notifications coalesce into one main-thread pass.
	work.Request();
	work.Request();
	assert(work.Take());
	assert(!work.Take());

	// A later notification re-arms the latch after consumption.
	work.Request();
	assert(work.Take());

	std::cout << "deferred_main_thread_work_tests: ok\n";
	return 0;
}
