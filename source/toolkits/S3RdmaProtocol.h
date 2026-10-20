// SPDX-FileCopyrightText: 2020-2025 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef TOOLKITS_S3RDMAPROTOCOL_H_
#define TOOLKITS_S3RDMAPROTOCOL_H_

// S3-over-RDMA wire protocol helpers (the "x-amz-rdma-*" header convention used
// by NVIDIA cuObject, and by Ceph RGW for libfabric tokens). Kept free of any AWS
// SDK, cuObject or libfabric dependency so the protocol logic stays independently
// testable.

// S3-over-RDMA needs S3 and one of its transports: cuObject or libfabric (ofi-rma)
#if defined(S3_SUPPORT) && (defined(CUOBJ_SUPPORT) || defined(OFI_SUPPORT) )
	#define S3RDMA_SUPPORT
#endif

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <string>
#include <system_error>
#include <sys/types.h>

namespace S3Rdma
{
	// S3 RDMA protocol headers (compatible with NVIDIA aws-c-s3 / cuObject).
	inline constexpr const char* AMZ_RDMA_TOKEN = "x-amz-rdma-token";
	inline constexpr const char* AMZ_RDMA_REPLY = "x-amz-rdma-reply";
	inline constexpr const char* AMZ_RDMA_BYTES_TRANSFERRED = "x-amz-rdma-bytes-transferred";
	/* the storage nodes' CRC-64/NVME of the bytes they placed (GET) or read (PUT), which a Ceph
		RGW returns for a transfer over libfabric; a range included */
	inline constexpr const char* AMZ_RDMA_CHECKSUM_CRC64NVME = "x-amz-rdma-checksum-crc64nvme";

	// SigV4 payload hash sentinel for body-less RDMA control-plane requests.
	inline constexpr const char* UNSIGNED_PAYLOAD = "UNSIGNED-PAYLOAD";

	// RDMA reply status codes (carried in x-amz-rdma-reply, aligned with HTTP codes).
	inline constexpr int RDMA_REPLY_SUCCESS = 200; // transfer completed (PUT/GET)
	inline constexpr int RDMA_REPLY_NO_CONTENT = 204; // transfer completed, no content (PUT)
	inline constexpr int RDMA_REPLY_PARTIAL_CONTENT = 206; // partial transfer (ranged GET)
	inline constexpr int RDMA_REPLY_NOT_IMPLEMENTED = 501; // server declined RDMA (hard error)

	// Return-code sentinels shared by rdmaPut/rdmaGet (negative => caller errors; no
	// HTTP fallback). >0 is the number of bytes transferred on success.
	inline constexpr ssize_t RDMA_NOT_SUPPORTED = -2; // server declined RDMA
	inline constexpr ssize_t RDMA_ERROR = -1; // transport / unexpected failure

	// One transient retry: a fresh token mint + control-plane attempt recovers from
	// a transient cuObject token-acquisition or transport hiccup.
	inline constexpr int RDMA_MAX_ATTEMPTS = 2;

	// Aggressive control-plane timeouts (seconds) so a transport stall surfaces fast
	// and the retry path can take over.
	inline constexpr long RDMA_CONNECT_TIMEOUT_SECS = 5;
	inline constexpr long RDMA_TIMEOUT_SECS = 10;

	/**
	 * Map the server's x-amz-rdma-reply header value to a transfer outcome.
	 *
	 *   >0  reply code (200/204/206): treat as RDMA success
	 *    0  unparsable non-empty value: treat as failure by the caller
	 *   -2  reply is "501" OR absent/empty: server declined RDMA
	 *
	 * A GET success carries x-amz-rdma-reply: 200/206, and a PUT success a 2xx
	 * with an ETag and x-amz-rdma-reply: 200/204. Its absence (a non-RDMA
	 * server never sets it) is read as a decline.
	 */
	inline int parseRdmaReply(const std::string& reply)
	{
		if(reply.empty() || reply == "501")
			return static_cast<int>(RDMA_NOT_SUPPORTED);

		// Require the ENTIRE value to be a valid integer; from_chars rejects
		// trailing junk ("200xyz" -> 200) that could mask a malformed reply.
		int value = 0;
		const char* begin = reply.data();
		const char* end = begin + reply.size();
		auto [parsedEnd, ec] = std::from_chars(begin, end, value);
		if(ec != std::errc{} || parsedEnd != end)
			return 0; // malformed -> caller treats as failure

		return value;
	}

	/**
	 * Whether a response that reports no RDMA success is a decline: the server did not take the
	 * token, so a client could send the payload over HTTP instead. That is x-amz-rdma-reply 501, or
	 * no x-amz-rdma-reply with HTTP 501 or with a status that does not fail the request (a server
	 * that ignores the token). An S3 error without the header, such as 400 BadDigest, 403
	 * AccessDenied or 404 NoSuchKey, is a failure of the request, not a decline.
	 */
	inline bool isRdmaDecline(int httpStatus, const std::string& reply)
	{
		if(reply == "501")
			return true;

		return reply.empty() && ( (httpStatus == 501) || (httpStatus < 400) );
	}

} // namespace S3Rdma

#endif // TOOLKITS_S3RDMAPROTOCOL_H_
