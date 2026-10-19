// SPDX-FileCopyrightText: 2020-2025 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef TOOLKITS_OFICLIENTTK_H_
#define TOOLKITS_OFICLIENTTK_H_

#ifdef OFI_SUPPORT

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

#include <ofi_rma/ofi_rma.h>

/**
 * Process-wide libfabric endpoint for S3-over-RDMA through ofi-rma, the counterpart of
 * SharedCuObjClient for S3 servers that take ofi-rma tokens, such as Ceph RGW with OSD-direct
 * transfers. Any libfabric provider works that the server's storage nodes also run, e.g. "tcp",
 * "verbs;ofi_rxm" or "uet".
 *
 * Each I/O buffer is registered once as a window that the storage nodes write into (GET) or read
 * from (PUT), and each request carries a token in the x-amz-rdma-token header that names its
 * window (see S3RdmaTk).
 *
 * A peer can deliver a copy of a write after the request that it belongs to completed, as a
 * provider that retransmits without state at the target (UET's RUDI mode) does. So the key of a
 * window is retired at the end of each request (endRequest()), before its buffer is used again:
 * a late copy then fails instead of landing.
 */
class SharedOfiClient
{
	public:
		/**
		 * Returns the process-wide endpoint, opened on the first call with the given provider,
		 * domain and local node address (empty: any). Later calls return the same endpoint,
		 * whatever they pass.
		 *
		 * @wantHmem ask the provider for FI_HMEM, so that windows can be in GPU memory.
		 * @return the endpoint, or NULL if it could not be opened.
		 */
		static SharedOfiClient* getInstance(const std::string& provider,
			const std::string& domain, const std::string& node, bool wantHmem);

		/**
		 * Register a buffer as a window. Peers may write into it, and read from it when the
		 * provider offers RMA reads (see canRead()).
		 *
		 * @isGPUMem the buffer is CUDA device memory of the given device.
		 * @return true on success.
		 */
		bool registerBuffer(void* ptr, size_t size, bool isGPUMem = false, int gpuID = 0);

		void deregisterBuffer(void* ptr);

		/**
		 * @return true if peers can read the windows (needed for PUT).
		 */
		bool canRead() const;

		/**
		 * @return true if windows can be in GPU memory.
		 */
		bool canUseGPUMem() const;

		/**
		 * The token that names [offset, offset+size) of ptr's window.
		 *
		 * @return the token, or an empty string on failure.
		 */
		std::string getToken(void* ptr, size_t size, size_t offset);

		/**
		 * The request that used ptr's window is over, whatever its outcome. Orders this thread's
		 * reads of the buffer after the provider's writes into it, and retires the window's key,
		 * so that no late write of the request lands in the buffer's next use. When the key cannot
		 * be retired, waits the endpoint's key quarantine instead.
		 */
		void endRequest(void* ptr);

		const std::string& getDescription() const { return description; }

	private:
		SharedOfiClient(const std::string& provider, const std::string& domain,
			const std::string& node, bool wantHmem);
		~SharedOfiClient();

		std::string lastError();

		struct ofi_rma_ep* ep{nullptr};
		std::map<void*, struct ofi_rma_window> windows; // by buffer
		std::mutex mutex; // protects windows
		std::string description;
};

#endif // OFI_SUPPORT

#endif // TOOLKITS_OFICLIENTTK_H_
