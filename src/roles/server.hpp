/*
 * Copyright (c) 2011, Peter Thorson. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the WebSocket++ Project nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" 
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE 
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE 
 * ARE DISCLAIMED. IN NO EVENT SHALL PETER THORSON BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * 
 */

#ifndef WEBSOCKETPP_ROLE_SERVER_HPP
#define WEBSOCKETPP_ROLE_SERVER_HPP

#include "../endpoint.hpp"
#include "../processors/hybi.hpp"
#include "../rng/blank_rng.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/scoped_ptr.hpp>

#include <iostream>
#include <stdexcept>

namespace websocketpp {
namespace role {

template <class endpoint>
class server {
public:
	// Connection specific details
	template <typename connection_type>
	class connection {
	public:
		typedef connection<connection_type> type;
		typedef endpoint endpoint_type;
		
		// Valid always
		unsigned int get_version() const {
			return m_version;
		}
		std::string get_request_header(const std::string& key) const {
			return m_request.header(key);
		}
		std::string get_origin() const {
			return m_origin;
		}
		
		// Information about the requested URI
		// valid only after URIs are loaded
		// TODO: check m_uri for NULLness
		bool get_secure() const {
			return m_uri->get_secure();
		}
		std::string get_host() const {
			return m_uri->get_host();
		}
		std::string get_resource() const {
			return m_uri->get_resource();
		}
		uint16_t get_port() const {
			return m_uri->get_port();
		}
		
		// Valid for CONNECTING state
		void add_response_header(const std::string& key, const std::string& value) {
			m_response.add_header(key,value);
		}
		void replace_response_header(const std::string& key, const std::string& value) {
			m_response.replace_header(key,value);
		}
		void remove_response_header(const std::string& key) {
			m_response.remove_header(key);
		}
		
		const std::vector<std::string>& get_subprotocols() const {
			return m_requested_subprotocols;
		}
		const std::vector<std::string>& get_extensions() const {
			return m_requested_extensions;
		}
		void select_subprotocol(const std::string& value) {
			std::vector<std::string>::iterator it;
			
			it = std::find(m_requested_subprotocols.begin(),
						   m_requested_subprotocols.end(),
						   value);
			
			if (value != "" && it == m_requested_subprotocols.end()) {
				throw std::invalid_argument("Attempted to choose a subprotocol not proposed by the client");
			}
			
			m_subprotocol = value;
		}
		void select_extension(const std::string& value) {
			if (value == "") {
				return;
			}
			
			std::vector<std::string>::iterator it;
			
			it = std::find(m_requested_extensions.begin(),
						   m_requested_extensions.end(),
						   value);
			
			if (it == m_requested_extensions.end()) {
				throw std::invalid_argument("Attempted to choose an extension not proposed by the client");
			}
			
			m_extensions.push_back(value);
		}
		
		// Valid if get_version() returns -1 (ie this is an http connection)
		void set_body(const std::string& value) {
			if (m_connection.m_version != -1) {
				// TODO: throw exception
				throw std::invalid_argument("set_body called from invalid state");
			}
			
			m_response.set_body(value);
		}
	protected:
		//connection(server<endpoint>& e) : m_endpoint(e) {}
		connection(endpoint& e)
		 : m_endpoint(e),
		   m_connection(static_cast< connection_type& >(*this)),
		   m_version(-1),
		   m_uri() {}
		
		// initializes the websocket connection
		void async_init() {
			boost::asio::async_read_until(
				m_connection.get_socket(),
				m_connection.buffer(),
				"\r\n\r\n",
				boost::bind(
					&type::handle_read_request,
					m_connection.shared_from_this(), // shared from this?
					boost::asio::placeholders::error,
					boost::asio::placeholders::bytes_transferred
				)
			);
		}
		
		void handle_read_request(const boost::system::error_code& error,
								 std::size_t bytes_transferred)
		{
			if (error) {
				// log error
				m_endpoint.elog().at(log::elevel::ERROR) << "Error reading HTTP request. code: " << error << log::endl;
				m_connection.terminate(false);
				return;
			}
			
			try {
				std::istream request(&m_connection.buffer());
				
				if (!m_request.parse_complete(request)) {
					// not a valid HTTP request/response
					throw http::exception("Recieved invalid HTTP Request",http::status_code::BAD_REQUEST);
				}
				
				m_endpoint.alog().at(log::alevel::DEBUG_HANDSHAKE) << m_request.raw() << log::endl;
								
				std::string h = m_request.header("Upgrade");
				if (boost::ifind_first(h,"websocket")) {
					h = m_request.header("Sec-WebSocket-Version");
					if (h == "") {
						// websocket upgrade is present but version is not.
						// assume hybi00
						m_version = 0;
					} else {
						m_version = atoi(h.c_str());
						if (m_version == 0) {
							throw(http::exception("Unable to determine connection version",http::status_code::BAD_REQUEST));
						}
					}
					
					// create a websocket processor
					if (m_version == 0) {
						m_response.add_header("Sec-WebSocket-Version","13, 8, 7");
						
						throw(http::exception("Unsupported WebSocket version",http::status_code::BAD_REQUEST));
					} else if (m_version == 7 ||
							   m_version == 8 ||
							   m_version == 13) {
						m_connection.m_processor = processor::ptr(new processor::hybi<blank_rng>(false,m_rng));
					} else {
						m_response.add_header("Sec-WebSocket-Version","13, 8, 7");
						
						throw(http::exception("Unsupported WebSocket version",http::status_code::BAD_REQUEST));
					}
					
					m_connection.m_processor->validate_handshake(m_request);
					m_origin = m_connection.m_processor->get_origin(m_request);
					m_uri = m_connection.m_processor->get_uri(m_request);
					
					m_endpoint.get_handler()->validate(m_connection.shared_from_this());
					
					m_response.set_status(http::status_code::SWITCHING_PROTOCOLS);
				} else {
					// continue as HTTP?
					m_endpoint.get_handler()->http(m_connection.shared_from_this());
					
					// should there be a more encapsulated http processor here?
					m_origin = m_request.header("Origin");
					
					// Set URI
					std::string h = m_request.header("Host");
					
					size_t found = h.find(":");
					if (found == std::string::npos) {
						// TODO: this makes the assumption that WS and HTTP
						// default ports are the same.
						m_uri.reset(new uri(m_endpoint.is_secure(),h,m_request.uri()));
					} else {
						m_uri.reset(new uri(m_endpoint.is_secure(),
									h.substr(0,found),
									h.substr(found+1),
									m_request.uri()));
					}
					
					m_response.set_status(http::status_code::OK);
				}
			} catch (const http::exception& e) {
				m_endpoint.elog().at(log::elevel::ERROR) << e.what() << log::endl;
				m_response.set_status(e.m_error_code,e.m_error_msg);
				m_response.set_body(e.m_body);
			} catch (const uri_exception& e) {
				// there was some error building the uri
				m_endpoint.elog().at(log::elevel::ERROR) << e.what() << log::endl;
				m_response.set_status(http::status_code::BAD_REQUEST);
			}
			
			write_response();
		}
		
		void write_response() {
			m_response.set_version("HTTP/1.1");
			
			if (m_response.status_code() == http::status_code::SWITCHING_PROTOCOLS) {
				// websocket response
				m_connection.m_processor->handshake_response(m_request,m_response);
				
				if (m_subprotocol != "") {
					m_response.replace_header("Sec-WebSocket-Protocol",m_subprotocol);
				}
				
				// TODO: return negotiated extensions
			} else {
				// HTTP response
			}
			
			m_response.replace_header("Server","WebSocket++/2011-11-18");
			
			std::string raw = m_response.raw();
			
			m_endpoint.alog().at(log::alevel::DEBUG_HANDSHAKE) << raw << log::endl;
			
			boost::asio::async_write(
				m_connection.get_socket(),
				boost::asio::buffer(raw),
				boost::bind(
					&type::handle_write_response,
					m_connection.shared_from_this(),
					boost::asio::placeholders::error
				)
			);
		}
		
		void handle_write_response(const boost::system::error_code& error) {
			// TODO: handshake timer
			
			if (error) {
				m_endpoint.elog().at(log::elevel::ERROR) << "Network error writing handshake respons. code: " << error << log::endl;
								
				m_connection.terminate(false);
				return;
			}
			
			log_open_result();
			
			if (m_response.status_code() != http::status_code::SWITCHING_PROTOCOLS) {
				if (m_version == -1) {
					// if this was not a websocket connection, we have written 
					// the expected response and the connection can be closed.
				} else {
					// this was a websocket connection that ended in an error
					m_endpoint.elog().at(log::elevel::ERROR) 
						<< "Handshake ended with HTTP error: " 
						<< m_response.status_code() << " " 
						<< m_response.status_msg() << log::endl;
				}
				m_connection.terminate(true);
				return;
			}
			
			m_connection.m_state = session::state::OPEN;
			
			m_endpoint.get_handler()->on_open(m_connection.shared_from_this());
			
			m_connection.handle_read_frame(boost::system::error_code());
		}
		
		void log_open_result() {
			std::stringstream version;
			version << "v" << m_version << " ";
			
			m_endpoint.alog().at(log::alevel::CONNECT) << (m_version == -1 ? "HTTP" : "WebSocket") << " Connection "
			<< m_connection.get_raw_socket().remote_endpoint() << " "
			<< (m_version == -1 ? "" : version.str())
			<< (get_request_header("User-Agent") == "" ? "NULL" : get_request_header("User-Agent")) 
			<< " " << m_uri->get_resource() << " " << m_response.status_code() 
			<< log::endl;
		}
		
	private:
		endpoint&					m_endpoint;
		connection_type&			m_connection;
		
		int							m_version;
		uri_ptr						m_uri;
		std::string					m_origin;
		std::vector<std::string>	m_requested_subprotocols;
		std::vector<std::string>	m_requested_extensions;
		std::string					m_subprotocol;
		std::vector<std::string>	m_extensions;
		
		http::parser::request		m_request;
		http::parser::response		m_response;
		blank_rng					m_rng;
	};
	
	class handler;
	
	typedef boost::shared_ptr<handler> handler_ptr;
	
	// types
	typedef server<endpoint> type;
	typedef endpoint endpoint_type;
	
	typedef typename endpoint_traits<endpoint>::connection_ptr connection_ptr;
	
	// handler interface callback class
	class handler {
	public:
		typedef connection_ptr foo;
		
		// Required
		virtual void validate(foo connection) = 0;
		virtual void on_open(foo connection) = 0;
		virtual void on_close(foo connection) = 0;
		
		virtual void on_message(foo connection,utf8_string_ptr) = 0;
		virtual void on_message(foo connection,binary_string_ptr) = 0;
		
		// Optional
		virtual bool on_ping(foo connection,binary_string_ptr) {return true;}
		virtual void on_pong(foo connection,binary_string_ptr) {}
		virtual void http(foo connection) {}
		virtual void on_fail(foo connection) {}
	};
	
	server(boost::asio::io_service& m,handler_ptr h) 
	 : m_ws_endpoint(static_cast< endpoint_type& >(*this)),
	   m_handler(h),
	   m_io_service(m),
	   m_endpoint(),
	   m_acceptor(m)
	{}
	
	void listen(unsigned short port) {
		m_endpoint.port(port);
		m_acceptor.open(m_endpoint.protocol());
		m_acceptor.set_option(boost::asio::socket_base::reuse_address(true));
		m_acceptor.bind(m_endpoint);
		m_acceptor.listen();
		
		this->start_accept();
		
		m_ws_endpoint.alog().at(log::alevel::DEVEL) << "role::server listening on port " << port << log::endl;
		
		m_io_service.run();
	}
protected:
	bool is_server() {
		return true;
	}
	
	handler_ptr get_handler() {
		return m_handler;
	}
private:
	// start_accept creates a new connection and begins an async_accept on it
	void start_accept() {
		connection_ptr con = m_ws_endpoint.create_connection();
		
		m_acceptor.async_accept(
			con->get_raw_socket(),
			boost::bind(
				&type::handle_accept,
				this,
				con,
				boost::asio::placeholders::error
			)
		);
	}
	
	// handle_accept will begin the connection's read/write loop and then reset
	// the server to accept a new connection. Errors returned by async_accept
	// are logged and ingored.
	void handle_accept(connection_ptr con, const boost::system::error_code& error) {
		if (error) {
			m_ws_endpoint.elog().at(log::elevel::ERROR) << "async_accept returned error: " << error << log::endl;
		} else {
			con->start();
		}
		
		this->start_accept();
	}
	
	endpoint_type&					m_ws_endpoint;
	handler_ptr						m_handler;
	boost::asio::io_service&		m_io_service;
	boost::asio::ip::tcp::endpoint	m_endpoint;
	boost::asio::ip::tcp::acceptor	m_acceptor;
};

	
} // namespace role	
} // namespace websocketpp

#endif // WEBSOCKETPP_ROLE_SERVER_HPP
