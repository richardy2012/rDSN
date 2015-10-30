/*
* The MIT License (MIT)
*
* Copyright (c) 2015 Microsoft Corporation
*
* -=- Robust Distributed System Nucleus (rDSN) -=-
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
* THE SOFTWARE.
*/


/************************************************************
*   hpc_logger (High-Performance Computing logger)
*
*   Editor: Chang Lou (v-chlou@microsoft.com)
*
*
*   The structure of the logger is like the following graph.
*
*   For each thread:
*   -------------------------              -------------------------
*   |    new single log     |     ->       |                       |
*   -------------------------              | --------------------- |
*                                          |                       |
*                                          | --------------------- |
*                                          |                       |
*                                          | --------------------- |
*                                          |                       |
*                                          | --------------------- |
*                                                    ...
*                                          | --------------------- |
*                                          |                       |
*                                          -------------------------
*                                             buffer (per thread)
*                                                      |
*                                                      |   when the buffer is full, 
*                                                      |   push the buffer and buffer size into _write_list,
*                                                      |   malloc a new buffer for the thread to use
*                                                      V
*                  ========================================================================================================== _write_list_lock
*
*                                            -------------------------------------------------------------      
*                                                {buf1, buf1_size} | {buf2, buf2_size} | {buf3, buf3_size} | ...     
*                                            -------------------------------------------------------------
*                                                              _write_list
*
*                  ========================================================================================================== _write_list_lock
*                                                                   |
*                                                                   |   when the _write_list is not empty,
*                                                                   |   daemon thread is notified by _write_list_cond
*	                                                                V          
*
*                                                             Daemon thread 
*
*                                                                   ||
*                                                                   ===========>     hpc_log.x.txt
*
*	Some other facts:
*	1. The log file size is restricted, when max size is achieved, a new log file will be established.
*	2. When exiting, the logger flushes, in other words, print out the retained log info in buffers of each thread and buffers in the buffer list.

************************************************************/

# include "hpc_logger.h"
# include <dsn/internal/singleton_store.h>
# include <dsn/cpp/utils.h>
# include <dsn/internal/command.h>
# include <cstdlib>
# include <sstream>
# include <fstream>
# include <iostream>


#define MAX_FILE_SIZE 30 * 1024 * 1024
namespace dsn
{
	namespace tools
	{
		//store log ptr for each thread
		typedef ::dsn::utils::safe_singleton_store<int, hpc_log_tls_info*> hpc_log_manager;

		//log ptr for each thread
		static __thread hpc_log_tls_info s_hpc_log_tls_info;

		//daemon thread
		void hpc_logger::log_thread()
		{
			_start_index = 0;
			_index = 0;

			// check existing log files and decide start_index
			std::vector<std::string> sub_list;
			std::string path = "./";
			if (!dsn::utils::filesystem::get_subfiles(path, sub_list, false))
			{
				dassert(false, "Fail to get subfiles in %s.", path.c_str());
			}

			for (auto& fpath : sub_list)
			{
				auto&& name = dsn::utils::filesystem::get_file_name(fpath);
				if (name.length() <= 10 ||
					name.substr(0, 8) != "hpc_log.")
					continue;

				int index;
				if (1 != sscanf(name.c_str(), "hpc_log.%d.txt", &index))
					continue;

				if (index > _index)
					_index = index;

				if (_start_index == 0 || index < _start_index)
					_start_index = index;
			}
			sub_list.clear();

			if (_start_index == 0)
				_start_index = _index;

			_index++;

			std::stringstream log;
			log << "hpc_log." << _index << ".txt";
			_current_log = new std::ofstream(log.str().c_str(), std::ofstream::out | std::ofstream::app | std::ofstream::binary);


			std::list<buffer_info*> saved_list;

			while (!_stop_thread || _flush_finish_flag==1)
			{
				_write_list_lock.lock();
				_write_list_cond.wait(_write_list_lock, [=]{ return  _stop_thread || _write_list.size() > 0; });
				saved_list = _write_list;
				_write_list.clear();
				_write_list_lock.unlock();
				
				if (_flush_finish_flag == true)
				{
					write_buffer_list(saved_list);
					_flush_finish_flag = false;
				}
				else
					write_buffer_list(saved_list);
			}

			_current_log->close();
			delete _current_log;
		}

		hpc_logger::hpc_logger() :_stop_thread(false), _flush_finish_flag(false)
		{

			_per_thread_buffer_bytes = config()->get_value<int>(
				"tools.hpc_logger",
				"per_thread_buffer_bytes",
				10 *1024* 1024, // 10 MB by default
				"buffer size for per-thread logging"
				);

			_log_thread = std::thread(&hpc_logger::log_thread, this);
			
		}

		hpc_logger::~hpc_logger(void)
		{
			if (!_stop_thread)
			{
				_stop_thread = true;
				_write_list_cond.notify_one();
				_log_thread.join();
			}
		}

		void hpc_logger::flush()
		{
			//print retained log in the buffers of threads
			hpc_log_flush_all_buffers_at_exit();
			_flush_finish_flag = true;

			_stop_thread = true;
			_write_list_cond.notify_one();
			_log_thread.join();
		}

		void hpc_logger::hpc_log_flush_all_buffers_at_exit()
		{
			std::vector<int> threads;
			hpc_log_manager::instance().get_all_keys(threads);

			for (auto& tid : threads)
			{
				__hpc_log_info__* log;
				if (!hpc_log_manager::instance().get(tid, log))
					continue;

				buffer_push(log->buffer, static_cast<int>(log->next_write_ptr - log->buffer));

				hpc_log_manager::instance().remove(tid);

			}

		}


		void hpc_logger::dsn_logv(const char *file,
			const char *function,
			const int line,
			dsn_log_level_t log_level,
			const char* title,
			const char *fmt,
			va_list args
			)
		{
			if (s_hpc_log_tls_info.magic != 0xdeadbeef)
			{
				s_hpc_log_tls_info.buffer = (char*)malloc(_per_thread_buffer_bytes);
				s_hpc_log_tls_info.next_write_ptr = s_hpc_log_tls_info.buffer;

				hpc_log_manager::instance().put(::dsn::utils::get_current_tid(), &s_hpc_log_tls_info);
				s_hpc_log_tls_info.magic = 0xdeadbeef;
			}

			// get enough write space >= 1K
			if (s_hpc_log_tls_info.next_write_ptr + 1024 > s_hpc_log_tls_info.buffer + _per_thread_buffer_bytes)
			{
				//critical section begin
				_write_list_lock.lock();

				buffer_push(s_hpc_log_tls_info.buffer, static_cast<int>(s_hpc_log_tls_info.next_write_ptr - s_hpc_log_tls_info.buffer));

				_write_list_cond.notify_one();
				_write_list_lock.unlock();
				//critical section end

				s_hpc_log_tls_info.buffer = (char*)malloc(_per_thread_buffer_bytes);
				s_hpc_log_tls_info.next_write_ptr = s_hpc_log_tls_info.buffer;
				

			}
			char* ptr = s_hpc_log_tls_info.next_write_ptr;
			char* ptr0 = ptr; // remember it
			size_t capacity = static_cast<size_t>(s_hpc_log_tls_info.buffer + _per_thread_buffer_bytes - ptr);

			// print verbose log header    
			uint64_t ts = 0;
			int tid = ::dsn::utils::get_current_tid();
			if (::dsn::tools::is_engine_ready())
				ts = dsn_now_ns();
			char str[24];
			::dsn::utils::time_ms_to_string(ts / 1000000, str);
			auto wn = sprintf(ptr, "%s (%llu %04x) ", str, static_cast<long long unsigned int>(ts), tid);
			ptr += wn;
			capacity -= wn;

			task* t = task::get_current_task();
			if (t)
			{
				if (nullptr != task::get_current_worker())
				{
					wn = sprintf(ptr, "%6s.%7s%u.%016llx: ",
						task::get_current_node_name(),
						task::get_current_worker()->pool_spec().name.c_str(),
						task::get_current_worker()->index(),
						static_cast<long long unsigned int>(t->id())
						);
				}
				else
				{
					wn = sprintf(ptr, "%6s.%7s.%05d.%016llx: ",
						task::get_current_node_name(),
						"io-thrd",
						tid,
						static_cast<long long unsigned int>(t->id())
						);
				}
			}
			else
			{
				wn = sprintf(ptr, "%6s.%7s.%05d: ",
					task::get_current_node_name(),
					"io-thrd",
					tid
					);
			}

			ptr += wn;
			capacity -= wn;

			// print body
			wn = std::vsnprintf(ptr, capacity, fmt, args);
			ptr += wn;
			capacity -= wn;

			// print body
			wn = std::sprintf(ptr, "%s", "\n");
			ptr += wn;
			capacity -= wn;

			// set next write ptr
			s_hpc_log_tls_info.next_write_ptr = ptr;

			// dump critical logs on screen
			if (log_level >= LOG_LEVEL_WARNING)
			{
				std::cout << ptr0 << std::endl;
			}	
		}
		//log operation

		void hpc_logger::buffer_push(char* buffer, int size)
		{
			buffer_info* new_buffer_info = (buffer_info*)malloc(sizeof(buffer_info));
			new_buffer_info->buffer = buffer;
			new_buffer_info->buffer_size = size;
			_write_list.push_back(new_buffer_info);
		}

		void hpc_logger::write_buffer_list(std::list<buffer_info*>& llist)
		{
			while (!llist.empty())
			{
				buffer_info* new_buffer_info = llist.front();

				if (_current_log_file_bytes + new_buffer_info->buffer_size >= MAX_FILE_SIZE)
				{

					_current_log_file_bytes = 0;
					_index++;

					_current_log->close();
					delete _current_log;

					std::stringstream log;
					log << "hpc_log." << _index << ".txt";
					_current_log = new std::ofstream(log.str().c_str(), std::ofstream::out | std::ofstream::app | std::ofstream::binary);
				}

				_current_log->write(new_buffer_info->buffer, new_buffer_info->buffer_size);

				_current_log_file_bytes += new_buffer_info->buffer_size;

				free(new_buffer_info->buffer);
				new_buffer_info->buffer = nullptr;
				free(new_buffer_info);
				new_buffer_info = nullptr;
				llist.pop_front();



			}


		}
	}
}