// SPDX-FileCopyrightText: 2020-2025 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "toolkits/OfiClientTk.h"

#ifdef OFI_SUPPORT

#include <chrono>
#include <cstdlib>
#include <thread>

#include "Logger.h"

SharedOfiClient::SharedOfiClient(const std::string& provider, const std::string& domain,
	const std::string& node, bool wantHmem)
{
	/* a provider's memory registration cache would hand a window that is registered again its
		old key, and closing a region would not revoke the key then (verbs and efa by default).
		ofi-rma needs the cache monitor off, before the first libfabric call. A user setting
		wins. */
	setenv("FI_MR_CACHE_MONITOR", "disabled", 0);

	struct ofi_rma_config cfg;
	ofi_rma_config_init(&cfg);
	cfg.provider = provider.c_str();
	cfg.domain = domain.empty() ? NULL : domain.c_str();
	cfg.node = node.empty() ? NULL : node.c_str();
	cfg.progress_thread = true; // places the server's writes while a worker waits for the reply
	cfg.reads = true; // PUT: the server reads the windows (best effort, see canRead())
	cfg.hmem = wantHmem; // GPU windows (best effort, see canUseGPUMem())

	char err[512] = "";
	const int rc = ofi_rma_ep_open(&cfg, &ep, err, sizeof(err) );
	if(rc < 0)
	{
		ERRLOGGER(Log_NORMAL, "S3 RDMA fabric not available (libfabric provider \"" <<
			provider << "\"): " << err << std::endl);
		ep = nullptr;
		return;
	}

	char desc[512] = "";
	ofi_rma_ep_describe(ep, desc, sizeof(desc) );
	description = desc;

	LOGGER(Log_NORMAL, "S3 RDMA fabric opened (libfabric): " << description <<
		(canRead() ? "" : "; no RMA reads, so no PUT") << std::endl);
}

SharedOfiClient::~SharedOfiClient()
{
	ofi_rma_ep_close(ep);
}

SharedOfiClient* SharedOfiClient::getInstance(const std::string& provider,
	const std::string& domain, const std::string& node, bool wantHmem)
{
	static SharedOfiClient instance(provider, domain, node, wantHmem);
	return instance.ep ? &instance : NULL;
}

std::string SharedOfiClient::lastError()
{
	char buf[512] = "";
	ofi_rma_ep_last_error(ep, buf, sizeof(buf) );
	return buf;
}

bool SharedOfiClient::canRead() const
{
	return ofi_rma_ep_reads(ep);
}

bool SharedOfiClient::canUseGPUMem() const
{
	return ofi_rma_ep_hmem(ep);
}

bool SharedOfiClient::registerBuffer(void* ptr, size_t size, bool isGPUMem, int gpuID)
{
	struct ofi_rma_memory mem;
	ofi_rma_memory_init(&mem);
	if(isGPUMem)
	{
		mem.iface = OFI_RMA_IFACE_CUDA;
		mem.device = gpuID;
	}

	const unsigned access = OFI_RMA_REMOTE_WRITE | (canRead() ? OFI_RMA_REMOTE_READ : 0);

	struct ofi_rma_window window;
	const int rc = ofi_rma_window_register(ep, ptr, size, &mem, access, &window);
	if(rc < 0)
	{
		ERRLOGGER(Log_NORMAL, "Registration of RDMA window failed. "
			"rc: " << rc << "; ptr: " << ptr << "; size: " << size << "; " << lastError() <<
			std::endl);
		return false;
	}

	const std::lock_guard<std::mutex> lock(mutex);
	windows[ptr] = window;

	return true;
}

void SharedOfiClient::deregisterBuffer(void* ptr)
{
	uint64_t id;

	{
		const std::lock_guard<std::mutex> lock(mutex);

		auto iter = windows.find(ptr);
		if(iter == windows.end() )
			return;

		id = iter->second.id;
		windows.erase(iter);
	}

	ofi_rma_window_deregister(ep, id);
}

std::string SharedOfiClient::getToken(void* ptr, size_t size, size_t offset)
{
	struct ofi_rma_window window;

	{
		const std::lock_guard<std::mutex> lock(mutex);

		auto iter = windows.find(ptr);
		if(iter == windows.end() )
		{
			ERRLOGGER(Log_NORMAL, "RDMA token requested for an unregistered buffer. "
				"ptr: " << ptr << std::endl);
			return "";
		}

		window = iter->second;
	}

	char token[1024];
	const ssize_t len = ofi_rma_window_token(ep, &window, offset, size, token, sizeof(token) );
	if(len < 0 || (size_t)len >= sizeof(token) )
	{
		ERRLOGGER(Log_NORMAL, "RDMA token for window failed. "
			"rc: " << len << "; ptr: " << ptr << "; offset: " << offset << "; size: " << size <<
			"; " << lastError() << std::endl);
		return "";
	}

	return std::string(token, len);
}

void SharedOfiClient::endRequest(void* ptr)
{
	struct ofi_rma_window window;

	{
		const std::lock_guard<std::mutex> lock(mutex);

		auto iter = windows.find(ptr);
		if(iter == windows.end() )
			return;

		window = iter->second;
	}

	// the reads of the buffer that follow see what the progress thread placed
	ofi_rma_ep_sync(ep);

	const int rc = ofi_rma_window_rekey(ep, &window);
	if(rc == 0)
		return;

	/* the window kept its key: a late write of this request could still land in the buffer, so
		leave it untouched as long as the endpoint's key quarantine */
	const int64_t quarantineMS = ofi_rma_ep_key_quarantine_ms(ep);

	ERRLOGGER(Log_VERBOSE, "Retiring the key of an RDMA window failed, waiting " <<
		quarantineMS << " ms before reusing it. rc: " << rc << "; ptr: " << ptr << "; " <<
		lastError() << std::endl);

	std::this_thread::sleep_for(std::chrono::milliseconds(quarantineMS) );
}

#endif // OFI_SUPPORT
