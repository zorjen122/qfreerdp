#pragma once

#include "qf_log.h"
#include "qf_util.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <utility>
#include <vector>

namespace qf
{
class clipboard_entry
{
public:
	explicit clipboard_entry(std::shared_ptr<client_t> client) : m_client(std::move(client)) {}
	~clipboard_entry() { stop_remote_file_worker(); }

	void enqueue_remote_file_list(CliprdrClientContext* context,
	                              const std::vector<clipboard_info_file_t>& files)
	{
		if (!context || files.empty())
			return;

		start_remote_file_worker();

		std::lock_guard<std::mutex> lock(m_remote_mutex);
		m_remote_context = context;
		for (size_t i = 0; i < files.size(); ++i)
		{
			if (files[i].is_directory_)
			{
				qf::log::info("cliprdr/remote-file", "skip directory name={}",
				              files[i].display_name_.toStdString());
				continue;
			}

			remote_file_job job;
			job.list_index = static_cast<UINT32>(i);
			job.display_name = files[i].display_name_;
			m_remote_jobs.push(job);
		}
		m_remote_cv.notify_one();
	}

	UINT server_file_contents_request(CliprdrClientContext* context,
	                                  const CLIPRDR_FILE_CONTENTS_REQUEST* request)
	{
		if (!context || !request || !m_client)
			return ERROR_INVALID_PARAMETER;

		if (request->listIndex >= m_client->clipboard_info_files_.size())
		{
			qf::log::warn("cliprdr/file-contents", "invalid listIndex={}", request->listIndex);
			return CB_RESPONSE_FAIL;
		}

		const auto& fileInfo = m_client->clipboard_info_files_[request->listIndex];

		if (request->dwFlags & FILECONTENTS_SIZE)
			return send_file_size(context, request->streamId, fileInfo);

		if (request->dwFlags & FILECONTENTS_RANGE)
			return send_file_range(context, request, fileInfo);

		qf::log::warn("cliprdr/file-contents", "unsupported dwFlags={}", request->dwFlags);
		return CB_RESPONSE_FAIL;
	}

	UINT server_file_contents_response(CliprdrClientContext* context,
	                                   const CLIPRDR_FILE_CONTENTS_RESPONSE* response)
	{
		if (!context || !response)
			return ERROR_INVALID_PARAMETER;

		std::lock_guard<std::mutex> lock(m_remote_mutex);
		if (!m_pending.active || response->streamId != m_pending.stream_id)
		{
			qf::log::warn("cliprdr/remote-file", "unexpected response streamId={}",
			              response->streamId);
			return CHANNEL_RC_OK;
		}

		m_pending.ok = response->common.msgFlags == CB_RESPONSE_OK;
		if (!m_pending.ok)
		{
			qf::log::warn("cliprdr/remote-file", "response failed streamId={} flags=0x{:04x}",
			              response->streamId, response->common.msgFlags);
		}
		m_pending.data.clear();
		if (m_pending.ok && response->requestedData && response->cbRequested > 0)
		{
			m_pending.data = QByteArray(reinterpret_cast<const char*>(response->requestedData),
			                            static_cast<qsizetype>(response->cbRequested));
		}
		m_pending.ready = true;
		m_remote_response_cv.notify_one();
		return CHANNEL_RC_OK;
	}

private:
	struct remote_file_job
	{
		UINT32 list_index = 0;
		QString display_name;
	};

	enum class remote_request_kind
	{
		none,
		size,
		range
	};

	struct remote_pending_response
	{
		bool active = false;
		bool ready = false;
		bool ok = false;
		UINT32 stream_id = 0;
		remote_request_kind kind = remote_request_kind::none;
		QByteArray data;
	};

	static UINT send_file_contents_response(CliprdrClientContext* context, UINT32 streamId,
	                                        const BYTE* data, UINT64 dataLen)
	{
		CLIPRDR_FILE_CONTENTS_RESPONSE response = WINPR_C_ARRAY_INIT;
		response.common.msgFlags = CB_RESPONSE_OK;
		response.streamId = streamId;
		response.cbRequested = dataLen;
		response.requestedData = data;

		return context->ClientFileContentsResponse(context, &response);
	}

	static UINT send_file_size(CliprdrClientContext* context, UINT32 streamId,
	                           const clipboard_info_file_t& fileInfo)
	{
		const uint64_t fileSize = fileInfo.is_directory_ ? 0 : fileInfo.total_;
		return send_file_contents_response(context, streamId,
		                                   reinterpret_cast<const BYTE*>(&fileSize),
		                                   sizeof(fileSize));
	}

	static UINT send_file_range(CliprdrClientContext* context,
	                            const CLIPRDR_FILE_CONTENTS_REQUEST* request,
	                            const clipboard_info_file_t& fileInfo)
	{
		if (fileInfo.is_directory_)
		{
			qf::log::warn("cliprdr/file-contents", "range requested for directory name={}",
			              fileInfo.display_name_.toStdString());
			return CB_RESPONSE_FAIL;
		}

		const uint64_t offset = (uint64_t(request->nPositionHigh) << 32) | request->nPositionLow;
		if (offset >= fileInfo.total_)
		{
			qf::log::warn("cliprdr/file-contents", "offset out of range offset={} size={}",
			              offset, fileInfo.total_);
			return CB_RESPONSE_FAIL;
		}

		QFile file(fileInfo.local_path_);
		if (!file.open(QIODevice::ReadOnly))
		{
			qf::log::warn("cliprdr/file-contents", "failed to open {}",
			              fileInfo.local_path_.toStdString());
			return CB_RESPONSE_FAIL;
		}

		if (!file.seek(offset))
		{
			qf::log::warn("cliprdr/file-contents", "failed to seek {} offset={}",
			              fileInfo.local_path_.toStdString(), offset);
			return CB_RESPONSE_FAIL;
		}

		const uint64_t remaining = static_cast<uint64_t>(file.size()) - offset;
		const uint64_t bytesToRead = std::min(remaining, uint64_t(request->cbRequested));

		QByteArray data(static_cast<qsizetype>(bytesToRead), Qt::Uninitialized);
		const qint64 bytesRead = file.read(data.data(), static_cast<qint64>(bytesToRead));
		if (bytesRead != static_cast<qint64>(bytesToRead))
		{
			qf::log::warn("cliprdr/file-contents", "failed to read {}",
			              fileInfo.local_path_.toStdString());
			return CB_RESPONSE_FAIL;
		}

		const UINT rc = send_file_contents_response(context, request->streamId,
		                                            reinterpret_cast<const BYTE*>(data.constData()),
		                                            bytesToRead);
		if (rc == CHANNEL_RC_OK)
			qf::log::info("cliprdr/file-contents", "sent range name={} offset={} bytes={}",
			              fileInfo.display_name_.toStdString(), offset, bytesToRead);
		return rc;
	}

	std::shared_ptr<client_t> m_client;

	void start_remote_file_worker()
	{
		if (m_remote_worker)
			return;

		m_remote_stopped = false;
		m_remote_worker = std::make_unique<std::thread>(&clipboard_entry::remote_file_worker_loop, this);
	}

	void stop_remote_file_worker()
	{
		{
			std::lock_guard<std::mutex> lock(m_remote_mutex);
			m_remote_stopped = true;
			m_pending.ready = true;
		}
		m_remote_cv.notify_all();
		m_remote_response_cv.notify_all();

		if (m_remote_worker && m_remote_worker->joinable())
			m_remote_worker->join();
		m_remote_worker.reset();
	}

	void remote_file_worker_loop()
	{
		while (true)
		{
			remote_file_job job;
			CliprdrClientContext* context = nullptr;
			{
				std::unique_lock<std::mutex> lock(m_remote_mutex);
				m_remote_cv.wait(lock, [&] { return m_remote_stopped || !m_remote_jobs.empty(); });
				if (m_remote_stopped)
					break;

				job = m_remote_jobs.front();
				m_remote_jobs.pop();
				context = m_remote_context;
			}

			if (!context)
				continue;
			download_remote_file(context, job);
		}
	}

	void download_remote_file(CliprdrClientContext* context, const remote_file_job& job)
	{
		uint64_t fileSize = 0;
		if (!request_remote_size(context, job.list_index, fileSize))
		{
			qf::log::warn("cliprdr/remote-file", "failed to query size name={}",
			              job.display_name.toStdString());
			return;
		}

		const QString localPath = make_remote_output_path(job.display_name, job.list_index);
		QFile file(localPath);
		if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
		{
			qf::log::warn("cliprdr/remote-file", "failed to create {}", localPath.toStdString());
			return;
		}

		uint64_t offset = 0;
		static constexpr UINT32 chunkSize = 64 * 1024;
		while (offset < fileSize)
		{
			const UINT32 bytesToRequest = static_cast<UINT32>(std::min<uint64_t>(chunkSize, fileSize - offset));
			QByteArray chunk;
			if (!request_remote_range(context, job.list_index, offset, bytesToRequest, chunk))
			{
				qf::log::warn("cliprdr/remote-file", "failed range name={} offset={}",
				              job.display_name.toStdString(), offset);
				return;
			}

			if (file.write(chunk.constData(), chunk.size()) != chunk.size())
			{
				qf::log::warn("cliprdr/remote-file", "failed write {}", localPath.toStdString());
				return;
			}
			offset += static_cast<uint64_t>(chunk.size());
		}

		qf::log::info("cliprdr/remote-file", "saved remote file name={} path={} bytes={}",
		              job.display_name.toStdString(), localPath.toStdString(), fileSize);
	}

	bool request_remote_size(CliprdrClientContext* context, UINT32 listIndex, uint64_t& size)
	{
		CLIPRDR_FILE_CONTENTS_REQUEST request = WINPR_C_ARRAY_INIT;
		request.streamId = next_remote_stream_id();
		request.listIndex = listIndex;
		request.dwFlags = FILECONTENTS_SIZE;
		request.cbRequested = sizeof(uint64_t);

		if (!send_remote_file_request(context, &request, remote_request_kind::size))
			return false;

		QByteArray data;
		if (!wait_remote_response(request.streamId, data) || data.size() < static_cast<int>(sizeof(uint64_t)))
			return false;

		std::memcpy(&size, data.constData(), sizeof(uint64_t));
		return true;
	}

	bool request_remote_range(CliprdrClientContext* context, UINT32 listIndex, uint64_t offset,
	                          UINT32 bytesToRequest, QByteArray& data)
	{
		CLIPRDR_FILE_CONTENTS_REQUEST request = WINPR_C_ARRAY_INIT;
		request.streamId = next_remote_stream_id();
		request.listIndex = listIndex;
		request.dwFlags = FILECONTENTS_RANGE;
		request.nPositionLow = static_cast<UINT32>(offset & 0xFFFFFFFFULL);
		request.nPositionHigh = static_cast<UINT32>((offset >> 32) & 0xFFFFFFFFULL);
		request.cbRequested = bytesToRequest;

		if (!send_remote_file_request(context, &request, remote_request_kind::range))
			return false;

		if (!wait_remote_response(request.streamId, data))
			return false;
		return static_cast<UINT32>(data.size()) == bytesToRequest;
	}

	bool send_remote_file_request(CliprdrClientContext* context,
	                              const CLIPRDR_FILE_CONTENTS_REQUEST* request,
	                              remote_request_kind kind)
	{
		{
			std::lock_guard<std::mutex> lock(m_remote_mutex);
			m_pending = {};
			m_pending.active = true;
			m_pending.stream_id = request->streamId;
			m_pending.kind = kind;
		}

		const UINT rc = context->ClientFileContentsRequest(context, request);
		if (rc != CHANNEL_RC_OK)
		{
			std::lock_guard<std::mutex> lock(m_remote_mutex);
			m_pending = {};
			qf::log::warn("cliprdr/remote-file", "request failed rc={} streamId={}",
			              rc, request->streamId);
			return false;
		}
		return true;
	}

	bool wait_remote_response(UINT32 streamId, QByteArray& data)
	{
		std::unique_lock<std::mutex> lock(m_remote_mutex);
		m_remote_response_cv.wait(lock, [&] {
			return m_remote_stopped || (m_pending.ready && m_pending.stream_id == streamId);
		});

		if (m_remote_stopped || !m_pending.ok)
		{
			m_pending = {};
			return false;
		}

		data = m_pending.data;
		m_pending = {};
		return true;
	}

	UINT32 next_remote_stream_id()
	{
		const UINT32 streamId = m_next_stream_id++;
		return streamId == 0 ? m_next_stream_id++ : streamId;
	}

	static QString make_remote_output_path(const QString& displayName, UINT32 listIndex)
	{
		QString normalized = displayName;
		normalized.replace("\\", "/");

		const QString fileName = QFileInfo(normalized).fileName();
		const QString safeName = fileName.isEmpty() ? QStringLiteral("clipboard-file") : fileName;
		const QString indexedName = QStringLiteral("%1-%2").arg(listIndex).arg(safeName);
		const QString dirPath = QDir::tempPath() + QStringLiteral("/qfreerdp-clipboard-") +
		                        QString::number(QCoreApplication::applicationPid());
		QDir().mkpath(dirPath);
		return QDir(dirPath).filePath(indexedName);
	}

	std::mutex m_remote_mutex;
	std::condition_variable m_remote_cv;
	std::condition_variable m_remote_response_cv;
	std::queue<remote_file_job> m_remote_jobs;
	remote_pending_response m_pending;
	CliprdrClientContext* m_remote_context = nullptr;
	std::atomic<bool> m_remote_stopped = false;
	std::unique_ptr<std::thread> m_remote_worker;
	UINT32 m_next_stream_id = 1;
};
} // namespace qf
