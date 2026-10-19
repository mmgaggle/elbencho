#ifdef BACKTRACE_SUPPORT
	#include <boost/stacktrace.hpp>
#endif

#ifdef CUDA_SUPPORT
	#include <cuda_runtime.h>
	#include <curand.h>
#endif

#ifdef CUFILE_SUPPORT
	#include <cufile.h>
#endif

#ifdef OFI_SUPPORT
	#include <ofi_rma/ofi_rma.h>
#endif

/**
 * Minimal program to be built as part of auto detection of available system libs etc.
 */
int main(int argc, char** argv)
{
#ifdef OFI_SUPPORT
	// links the library, not just its header
	if(argc > 1000)
		return (int)ofi_rma_version();
#endif

	return 0;
}
