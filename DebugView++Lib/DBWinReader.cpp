// (C) Copyright Gert-Jan de Vos and Jan Wilmans 2013.
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at 
// http://www.boost.org/LICENSE_1_0.txt)

// Repository at: https://github.com/djeedjay/DebugViewPP/

#include "stdafx.h"
#include "DebugView++Lib/DBWinBuffer.h"
#include "DebugView++Lib/DBWinReader.h"
#include "DebugView++Lib/ProcessInfo.h"
#include "DebugView++Lib/LineBuffer.h"

namespace fusion {
namespace debugviewpp {

const double HandleCacheTimeout = 15.0; //seconds

std::wstring GetDBWinName(bool global, const std::wstring& name)
{
	return global ? L"Global\\" + name : name;
}

Handle CreateDBWinBufferMapping(bool global)
{
	Handle hMap(CreateFileMapping(nullptr, nullptr, PAGE_READWRITE, 0, sizeof(DbWinBuffer), GetDBWinName(global, L"DBWIN_BUFFER").c_str()));
	if (GetLastError() == ERROR_ALREADY_EXISTS)
		throw std::runtime_error("CreateDBWinBufferMapping");
	return hMap;
}

DBWinReader::DBWinReader(ILineBuffer& linebuffer, bool global) :
	LogSource(SourceType::System, linebuffer),
	m_hBuffer(CreateDBWinBufferMapping(global)),
	m_dbWinBufferReady(CreateEvent(nullptr, false, true, GetDBWinName(global, L"DBWIN_BUFFER_READY").c_str())),
	m_dbWinDataReady(CreateEvent(nullptr, false, false, GetDBWinName(global, L"DBWIN_DATA_READY").c_str())),
	m_mappedViewOfFile(m_hBuffer.get(), PAGE_READONLY, 0, 0, sizeof(DbWinBuffer)),
	m_dbWinBuffer(static_cast<const DbWinBuffer*>(m_mappedViewOfFile.Ptr())),
	m_handleCacheTime(0.0)
{
	SetDescription(global ? L"Global Win32 Messages" : L"Win32 Messages");
	m_lines.reserve(4000);
	m_backBuffer.reserve(4000);
	SetEvent(m_dbWinBufferReady.get());
}

DBWinReader::~DBWinReader()
{
	Abort();
}

bool DBWinReader::AtEnd() const
{
	return false;
}

HANDLE DBWinReader::GetHandle() const 
{
	return m_dbWinDataReady.get();
}

void DBWinReader::Notify()
{
	HANDLE handle = ::OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, m_dbWinBuffer->processId);

#ifdef OPENPROCESS_DEBUG
	if (handle == 0)
	{
		Win32Error error(GetLastError(), "OpenProcess");
		std::string s = stringbuilder() << error.what() << " " <<  m_dbWinBuffer->data;
		Add(m_dbWinBuffer->processId, s.c_str(), handle);
		continue;
	}
#endif
	Add(m_dbWinBuffer->processId, m_dbWinBuffer->data, handle);
	SetEvent(m_dbWinBufferReady.get());
}

void DBWinReader::Abort()
{
}

#ifdef USE_NEW_LOGSOURCE_PATH
void DBWinReader::Add(DWORD pid, const char* text, HANDLE handle)
{
	LogSource::Add(m_timer.Get(), GetSystemTimeAsFileTime(), handle, text);
}

#else

void DBWinReader::Add(DWORD pid, const char* text, HANDLE handle)
{
	DBWinMessage line;
	line.time = m_timer.Get();
	line.systemTime = GetSystemTimeAsFileTime();
	line.pid = pid;
	line.handle = handle;
	line.message = text;
	AddLine(line);
}
#endif

void DBWinReader::AddLine(const DBWinMessage& DBWinMessage)		// depricated, remove
{
	boost::unique_lock<boost::mutex> lock(m_linesMutex);
	m_lines.push_back(DBWinMessage);
}

Lines DBWinReader::GetLines()									// depricated, remove
{
	m_backBuffer.clear();
	{
		boost::unique_lock<boost::mutex> lock(m_linesMutex);
		m_lines.swap(m_backBuffer);
	}
	return ProcessLines(m_backBuffer);
}

Lines DBWinReader::ProcessLines(const DBWinMessages& DBWinMessages)
{
	Lines resolvedLines = CheckHandleCache();
	for (auto i = DBWinMessages.begin(); i != DBWinMessages.end(); ++i)
	{
		std::string processName; 
		if (i->handle)
		{
			Handle processHandle(i->handle);
			processName = Str(ProcessInfo::GetProcessName(processHandle.get())).str();
			m_handleCache.Add(i->pid, std::move(processHandle));
		}

		auto lines = ProcessLine(Line(i->time, i->systemTime, i->pid, processName, i->message));
		for (auto line = lines.begin(); line != lines.end(); ++line)
			resolvedLines.push_back(*line);
	}

	return resolvedLines;
}

Lines DBWinReader::ProcessLine(const Line& line)
{
	Lines lines;
	if (m_lineBuffers.find(line.pid) == m_lineBuffers.end())
	{
		std::string message;
		message.reserve(4000);
		m_lineBuffers[line.pid] = std::move(message);
	}
	std::string& message = m_lineBuffers[line.pid];

	Line outputLine = line;
	for (auto i = line.message.begin(); i != line.message.end(); i++)
	{
		if (*i == '\r')
			continue;

		if (*i == '\n')
		{
			outputLine.message = std::move(message);
			message.clear();
			lines.push_back(outputLine);
		}
		else
		{
			message.push_back(char(*i));
		}
	}

	if (message.empty())
	{
		m_lineBuffers.erase(line.pid);
	}
	else if (GetAutoNewLine() || message.size() > 8192)	// 8k line limit prevents stack overflow in handling code 
	{
		outputLine.message = std::move(message);
		message.clear();
		lines.push_back(outputLine);
	}
	return lines;
}

Lines DBWinReader::CheckHandleCache()
{
	if ((m_timer.Get() - m_handleCacheTime) < HandleCacheTimeout)
		return Lines();

	Lines lines;
	Pids removedPids = m_handleCache.Cleanup();
	for (auto i = removedPids.begin(); i != removedPids.end(); i++)
	{
		DWORD pid = *i;
		if (m_lineBuffers.find(pid) != m_lineBuffers.end())
		{
			if (!m_lineBuffers[pid].empty())
				lines.push_back(Line(m_timer.Get(), GetSystemTimeAsFileTime(), pid, "<flush>", m_lineBuffers[pid]));
			m_lineBuffers.erase(pid);
		}
	}
	m_handleCacheTime = m_timer.Get();
	return lines;
}

} // namespace debugviewpp 
} // namespace fusion
