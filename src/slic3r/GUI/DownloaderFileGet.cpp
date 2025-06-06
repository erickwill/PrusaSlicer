///|/ Copyright (c) Prusa Research 2023 Oleksandra Iushchenko @YuSanka, David Kocík @kocikdav
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "DownloaderFileGet.hpp"

#include <thread>
#include <curl/curl.h>
#include <boost/nowide/fstream.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/nowide/cstdio.hpp>
#include <iostream>
#include <regex>

#include "format.hpp"
#include "GUI.hpp"
#include "I18N.hpp"

namespace Slic3r {
namespace GUI {

const size_t DOWNLOAD_MAX_CHUNK_SIZE	= 10 * 1024 * 1024;
const size_t DOWNLOAD_SIZE_LIMIT		= 1024 * 1024 * 1024;

bool FileGet::is_subdomain(const std::string& url, const std::string& domain)
{
	// domain should be f.e. printables.com (.com including)
	char* host;
	std::string host_string;
	CURLUcode rc;
	CURLU* curl = curl_url();
	if (!curl) {
		BOOST_LOG_TRIVIAL(error) << "Failed to init Curl library in function is_domain.";
		return false;
	}
	rc = curl_url_set(curl, CURLUPART_URL, url.c_str(), 0);
	if (rc != CURLUE_OK) {
		curl_url_cleanup(curl);
		return false;
	}
	rc = curl_url_get(curl, CURLUPART_HOST, &host, 0);
	if (rc != CURLUE_OK || !host) {
		curl_url_cleanup(curl);
		return false;
	}
	host_string = std::string(host);
	curl_free(host);
	// now host should be subdomain.domain or just domain
	if (domain == host_string) {
		curl_url_cleanup(curl);
		return true;
	}
	if(boost::ends_with(host_string, "." + domain)) {
		curl_url_cleanup(curl);
		return true;
	}
	curl_url_cleanup(curl);
	return false;
}

namespace {
unsigned get_current_pid()
{
#ifdef WIN32
	return GetCurrentProcessId();
#else
	return ::getpid();
#endif
}

std::string extract_filename_from_header(const std::string& headers) {
    // Split the headers into lines
    std::istringstream header_stream(headers);
    std::string line;

    while (std::getline(header_stream, line)) {
        if (line.find("content-disposition") != std::string::npos) {
            // Apply regex to extract filename from the content-disposition line
            std::regex filename_regex("filename\\s*=\\s*\"([^\"]+)\"", std::regex::icase);
            std::smatch match;

            if (std::regex_search(line, match, filename_regex) && match.size() > 1) {
                return match.str(1);
            }
        }
    }

    return {};
}
}

// int = DOWNLOAD ID; string = file path
wxDEFINE_EVENT(EVT_DWNLDR_FILE_COMPLETE, Event<DownloadEventData>);
// int = DOWNLOAD ID; string = error msg
wxDEFINE_EVENT(EVT_DWNLDR_FILE_ERROR, wxCommandEvent);
// int = DOWNLOAD ID; string = progress percent
wxDEFINE_EVENT(EVT_DWNLDR_FILE_PROGRESS, wxCommandEvent);
// int = DOWNLOAD ID; string = name
wxDEFINE_EVENT(EVT_DWNLDR_FILE_NAME_CHANGE, wxCommandEvent);
// int = DOWNLOAD ID; 
wxDEFINE_EVENT(EVT_DWNLDR_FILE_PAUSED, wxCommandEvent);
// int = DOWNLOAD ID; 
wxDEFINE_EVENT(EVT_DWNLDR_FILE_CANCELED, wxCommandEvent);

struct FileGet::priv
{
	const int m_id;
	std::string m_url;
	std::string m_filename;
	std::thread m_io_thread;
	wxEvtHandler* m_evt_handler;
	boost::filesystem::path m_dest_folder;
	boost::filesystem::path m_tmp_path; // path when ongoing download
	std::atomic_bool m_cancel { false };
	std::atomic_bool m_pause  { false };
	std::atomic_bool m_stopped { false }; // either canceled or paused - download is not running
	size_t m_written { 0 };
	size_t m_absolute_size { 0 };
    bool m_load_after;
	priv(int ID, std::string&& url, const std::string& filename, wxEvtHandler* evt_handler, const boost::filesystem::path& dest_folder, bool load_after);

	void get_perform();
};

FileGet::priv::priv(int ID, std::string&& url, const std::string& filename, wxEvtHandler* evt_handler, const boost::filesystem::path& dest_folder, bool load_after)
	: m_id(ID)
	, m_url(std::move(url))
	, m_filename(filename)
	, m_evt_handler(evt_handler)
	, m_dest_folder(dest_folder)
    , m_load_after(load_after)
{
    // Prevent ':' in filename
    m_filename.erase(std::remove(m_filename.begin(), m_filename.end(), ':'), m_filename.end());
}

void FileGet::priv::get_perform()
{
	assert(m_evt_handler);
	assert(!m_url.empty());
	assert(!m_filename.empty());
	assert(boost::filesystem::is_directory(m_dest_folder));

	m_stopped = false;

	// open dest file
	if (m_written == 0)
	{
		boost::filesystem::path dest_path = m_dest_folder / m_filename;
		std::string extension = dest_path.extension().string();
		std::string just_filename = m_filename.substr(0, m_filename.size() - extension.size());
		std::string final_filename = just_filename;
        // Find unused filename 
		try {
			size_t version = 0;
			while (boost::filesystem::exists(m_dest_folder / (final_filename + extension)) || boost::filesystem::exists(m_dest_folder / (final_filename + extension + "." + std::to_string(get_current_pid()) + ".download")))
			{
				++version;
				if (version > 999) {
					wxCommandEvent* evt = new wxCommandEvent(EVT_DWNLDR_FILE_ERROR);
					evt->SetString(GUI::format_wxstr(L"Failed to find suitable filename. Last name: %1%." , (m_dest_folder / (final_filename + extension)).string()));
					evt->SetInt(m_id);
					m_evt_handler->QueueEvent(evt);
					return;
				}
				final_filename = GUI::format("%1%(%2%)", just_filename, std::to_string(version));
			}
		} catch (const boost::filesystem::filesystem_error& e)
		{
			wxCommandEvent* evt = new wxCommandEvent(EVT_DWNLDR_FILE_ERROR);
			evt->SetString(e.what());
			evt->SetInt(m_id);
			m_evt_handler->QueueEvent(evt);
			return;
		}
		
		m_filename = final_filename + extension;
		m_tmp_path = m_dest_folder / (m_filename + "." + std::to_string(get_current_pid()) + ".download");

		wxCommandEvent* evt = new wxCommandEvent(EVT_DWNLDR_FILE_NAME_CHANGE);
		evt->SetString(from_u8(m_filename));
		evt->SetInt(m_id);
		m_evt_handler->QueueEvent(evt);
	}
	
	boost::filesystem::path dest_path = m_dest_folder / m_filename;
	
	BOOST_LOG_TRIVIAL(info) << GUI::format("Starting download from %1% to %2%. Temp path is %3%",m_url, dest_path, m_tmp_path);

	FILE* file;
	// open file for writting
	if (m_written == 0)
		file = boost::nowide::fopen(m_tmp_path.string().c_str(), "wb");
	else 
		file = boost::nowide::fopen(m_tmp_path.string().c_str(), "ab");

	//assert(file != NULL);
	if (file == NULL) {
		wxCommandEvent* evt = new wxCommandEvent(EVT_DWNLDR_FILE_ERROR);
		// TRN %1% = file path
		evt->SetString(GUI::format_wxstr(_L("Can't create file at %1%"), m_tmp_path.string()));
		evt->SetInt(m_id);
		m_evt_handler->QueueEvent(evt);
		return;
	}

	std:: string range_string = std::to_string(m_written) + "-";

	size_t written_previously = m_written;
	size_t written_this_session = 0;
	Http::get(m_url)
		.size_limit(DOWNLOAD_SIZE_LIMIT) //more?
		.set_range(range_string)
		.on_progress([&](Http::Progress progress, bool& cancel) {
			// to prevent multiple calls into following ifs (m_cancel / m_pause)
			if (m_stopped){
				cancel = true;
				return;
			}
			if (m_cancel) {
				m_stopped = true;
				fclose(file);
				// remove canceled file
				std::remove(m_tmp_path.string().c_str());
				m_written = 0;
				cancel = true;
				wxCommandEvent* evt = new wxCommandEvent(EVT_DWNLDR_FILE_CANCELED);
				evt->SetInt(m_id);
				m_evt_handler->QueueEvent(evt);
				return;
				// TODO: send canceled event?
			}		
			if (m_pause) {
				m_stopped = true;
				fclose(file);
				cancel = true;
				if (m_written == 0)
					std::remove(m_tmp_path.string().c_str());
				wxCommandEvent* evt = new wxCommandEvent(EVT_DWNLDR_FILE_PAUSED);
				evt->SetInt(m_id);
				m_evt_handler->QueueEvent(evt);
				return;
			}
			
			if (m_absolute_size < progress.dltotal) {
				m_absolute_size = progress.dltotal;
			}

			if (progress.dlnow != 0) {
				if (progress.dlnow - written_this_session > DOWNLOAD_MAX_CHUNK_SIZE || progress.dlnow == progress.dltotal) {
					try
					{
						std::string part_for_write = progress.buffer.substr(written_this_session, progress.dlnow);
						fwrite(part_for_write.c_str(), 1, part_for_write.size(), file);
					}
					catch (const std::exception& e)
					{
						// fclose(file); do it?
						wxCommandEvent* evt = new wxCommandEvent(EVT_DWNLDR_FILE_ERROR);
						evt->SetString(e.what());
						evt->SetInt(m_id);
						m_evt_handler->QueueEvent(evt);
						cancel = true;
						return;
					}
					written_this_session = progress.dlnow;
					m_written = written_previously + written_this_session;
				}
				wxCommandEvent* evt = new wxCommandEvent(EVT_DWNLDR_FILE_PROGRESS);
				int percent_total = m_absolute_size == 0 ? 0 : (written_previously + progress.dlnow) * 100 / m_absolute_size;
				evt->SetString(std::to_string(percent_total));
				evt->SetInt(m_id);
				m_evt_handler->QueueEvent(evt);
			}
			
		})
        .on_headers([&](const std::string& headers) {
            // we are looking for content-disposition header in response, to use it as correct filename
            std::string new_filename = extract_filename_from_header(headers);
            if (new_filename.empty()) {
                return;
            }
            // Find unused filename
            boost::filesystem::path temp_dest_path = m_dest_folder / new_filename;
		    std::string extension = temp_dest_path.extension().string();
		    std::string just_filename = new_filename.substr(0, new_filename.size() - extension.size());
		    std::string final_filename = just_filename;
            try {
			    size_t version = 0;
			    while (boost::filesystem::exists(m_dest_folder / (final_filename + extension)))
			    {
				    ++version;
				    if (version > 999) {
                        BOOST_LOG_TRIVIAL(error) << GUI::format("Failed to find suitable filename. Last name: %1%." , (m_dest_folder / (final_filename + extension)).string());
					    return;
				    }
				    final_filename = GUI::format("%1%(%2%)", just_filename, std::to_string(version));
			    }
		    } catch (const boost::filesystem::filesystem_error& e)
		    {
			    BOOST_LOG_TRIVIAL(error) << "Failed to resolved filename from headers.";
			    return;
		    }
            m_filename = final_filename + extension;
            dest_path = m_dest_folder / m_filename;

            wxCommandEvent* evt = new wxCommandEvent(EVT_DWNLDR_FILE_NAME_CHANGE);
		    evt->SetString(from_u8(m_filename));
		    evt->SetInt(m_id);
		    m_evt_handler->QueueEvent(evt);
        })
		.on_error([&](std::string body, std::string error, unsigned http_status) {
			if (file != NULL)
				fclose(file);
			wxCommandEvent* evt = new wxCommandEvent(EVT_DWNLDR_FILE_ERROR);
			if (!error.empty())
				evt->SetString(GUI::from_u8(error));
			else
				evt->SetString(GUI::from_u8(body));
			evt->SetInt(m_id);
			m_evt_handler->QueueEvent(evt);
		})
		.on_complete([&](std::string body, unsigned /* http_status */) {
			try
			{
                // If server is not sending Content-Length header, the progress function does not write all data to file.
                // We need to write it now.
                if (written_this_session < body.size())  {
                    std::string part_for_write = body.substr(written_this_session);
				    fwrite(part_for_write.c_str(), 1, part_for_write.size(), file);
                }
				fclose(file);
				boost::filesystem::rename(m_tmp_path, dest_path);
			}
			catch (const std::exception& e)
			{
				//TODO: report?
				//error_message = GUI::format("Failed to write and move %1% to %2%", tmp_path, dest_path);
				wxCommandEvent* evt = new wxCommandEvent(EVT_DWNLDR_FILE_ERROR);
				evt->SetString(GUI::format("Failed to write and move %1% to %2%", m_tmp_path, dest_path));
				evt->SetInt(m_id);
				m_evt_handler->QueueEvent(evt);
				return;
			}
            wxCommandEvent* evt = new wxCommandEvent(EVT_DWNLDR_FILE_PROGRESS);
			int percent_total = 100;
			evt->SetString(std::to_string(percent_total));
			evt->SetInt(m_id);
			m_evt_handler->QueueEvent(evt);

            DownloadEventData event_data = {m_id, dest_path.wstring(), m_load_after};
            wxQueueEvent(m_evt_handler, new Event<DownloadEventData>(EVT_DWNLDR_FILE_COMPLETE, event_data));
		})
		.perform_sync();

}

FileGet::FileGet(int ID, std::string url, const std::string& filename, wxEvtHandler* evt_handler, const boost::filesystem::path& dest_folder, bool load_after)
	: p(new priv(ID, std::move(url), filename, evt_handler, dest_folder, load_after))
{}

FileGet::FileGet(FileGet&& other) : p(std::move(other.p)) {}

FileGet::~FileGet()
{
	if (p && p->m_io_thread.joinable()) {
		p->m_cancel = true;
		p->m_io_thread.join();
	}
}

void FileGet::get()
{
	assert(p);
	if (p->m_io_thread.joinable()) {
			// This will stop transfers being done by the thread, if any.
			// Cancelling takes some time, but should complete soon enough.
			p->m_cancel = true;
			p->m_io_thread.join();
	}
	p->m_cancel = false;
	p->m_pause = false;
	p->m_io_thread = std::thread([this]() {
		p->get_perform();
		});
}

void FileGet::cancel()
{
	if(p && p->m_stopped) {
		if (p->m_io_thread.joinable()) {
			p->m_cancel = true;
			p->m_io_thread.join();
			wxCommandEvent* evt = new wxCommandEvent(EVT_DWNLDR_FILE_CANCELED);
			evt->SetInt(p->m_id);
			p->m_evt_handler->QueueEvent(evt);
		}
	}

	if (p)
		p->m_cancel = true;
	
}

void FileGet::pause()
{
	if (p) {
		p->m_pause = true;
	}
}
void FileGet::resume()
{
	assert(p);
	if (p->m_io_thread.joinable()) {
		// This will stop transfers being done by the thread, if any.
		// Cancelling takes some time, but should complete soon enough.
		p->m_cancel = true;
		p->m_io_thread.join();
	}
	p->m_cancel = false;
	p->m_pause = false;
	p->m_io_thread = std::thread([this]() {
		p->get_perform();
		});
}
}
}
