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
#include "rdp-view-item.h"

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

		auto batch = std::make_shared<remote_file_batch>();
		batch->root_path = make_remote_batch_root();
		if (!QDir().mkpath(batch->root_path))
		{
			qf::log::warn("cliprdr/remote-file", "failed to create batch root {}",
			              batch->root_path.toStdString());
			return;
		}

		for (size_t i = 0; i < files.size(); ++i)
		{
			const QString relativePath = safe_remote_relative_path(files[i].display_name_);
			if (relativePath.isEmpty())
			{
				qf::log::warn("cliprdr/remote-file", "reject unsafe path name={}",
				              files[i].display_name_.toStdString());
				continue;
			}

			const QString localPath = QDir(batch->root_path).filePath(relativePath);
			const QString topName = relativePath.section('/', 0, 0);
			const QString topPath = QDir(batch->root_path).filePath(topName);
			if (std::find(batch->top_paths.begin(), batch->top_paths.end(), topPath) ==
			    batch->top_paths.end())
			{
				batch->top_paths.push_back(topPath);
			}

			if (files[i].is_directory_)
			{
				if (!QDir().mkpath(localPath))
				{
					qf::log::warn("cliprdr/remote-file", "failed to create directory {}",
					              localPath.toStdString());
				}
				else
				{
					qf::log::debug("cliprdr/remote-file", "created directory path={}",
					               localPath.toStdString());
				}
				continue;
			}

			if (!QDir().mkpath(QFileInfo(localPath).absolutePath()))
			{
				qf::log::warn("cliprdr/remote-file", "failed to create parent for {}",
				              localPath.toStdString());
				continue;
			}

			remote_file_job job;
			job.list_index = static_cast<UINT32>(i);
			job.display_name = files[i].display_name_;
			job.local_path = localPath;
			job.batch = batch;
			m_remote_jobs.push(job);
			++batch->remaining;
		}

		if (batch->remaining == 0)
			publish_remote_batch(batch);
		else
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
	struct remote_file_batch
	{
		size_t remaining = 0;
		QString root_path;
		std::vector<QString> top_paths;
	};

	struct remote_file_job
	{
		UINT32 list_index = 0;
		QString display_name;
		QString local_path;
		std::shared_ptr<remote_file_batch> batch;
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
			if (--job.batch->remaining == 0)
				publish_remote_batch(job.batch);
		}
	}

	QString download_remote_file(CliprdrClientContext* context, const remote_file_job& job)
	{
		// protocol: [SIZE flag the pdu] [RANGE flag the pdu]
		uint64_t fileSize = 0;
		if (!request_remote_size(context, job.list_index, fileSize))
		{
			qf::log::warn("cliprdr/remote-file", "failed to query size name={}",
			              job.display_name.toStdString());
			return {};
		}

		QFile file(job.local_path);
		if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
		{
			qf::log::warn("cliprdr/remote-file", "failed to create {}",
			              job.local_path.toStdString());
			return {};
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
				file.close();
				QFile::remove(job.local_path);
				return {};
			}

			if (file.write(chunk.constData(), chunk.size()) != chunk.size())
			{
				qf::log::warn("cliprdr/remote-file", "failed write {}",
				              job.local_path.toStdString());
				file.close();
				QFile::remove(job.local_path);
				return {};
			}
			offset += static_cast<uint64_t>(chunk.size());
		}

		qf::log::info("cliprdr/remote-file", "saved remote file name={} path={} bytes={}",
		              job.display_name.toStdString(), job.local_path.toStdString(), fileSize);
		return job.local_path;
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

	static QString safe_remote_relative_path(const QString& displayName)
	{
		QString normalized = displayName;
		normalized.replace("\\", "/");

		if (normalized.startsWith('/') ||
		    (normalized.size() >= 2 && normalized[1] == ':'))
			return {};

		const QStringList parts = normalized.split('/', Qt::SkipEmptyParts);
		if (parts.empty())
			return {};

		for (const QString& part : parts)
		{
			if (part == "." || part == "..")
				return {};
		}

		return QDir::cleanPath(parts.join('/'));
	}

	QString make_remote_batch_root()
	{
		const QString name =
		    QStringLiteral("qfreerdp-clipboard-%1-%2")
		        .arg(QCoreApplication::applicationPid())
		        .arg(m_next_batch_id++);
		return QDir::temp().filePath(name);
	}

	void publish_remote_batch(const std::shared_ptr<remote_file_batch>& batch)
	{
		if (!batch || batch->top_paths.empty() || !m_client || !m_client->rdpViewItem)
			return;

		const auto paths = batch->top_paths;
		RdpViewItem* viewItem = m_client->rdpViewItem;
		QMetaObject::invokeMethod(
		    viewItem,
		    [viewItem, paths] { viewItem->updateClipboardFilesFromRemote(paths); },
		    Qt::QueuedConnection);
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
	uint64_t m_next_batch_id = 1;
};
} // namespace qf
