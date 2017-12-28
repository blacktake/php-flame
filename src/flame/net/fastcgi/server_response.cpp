#include "../../coroutine.h"
#include "../../log/log.h"
#include "../http/http.h"
#include "../http/server_connection_base.h"
#include "fastcgi.h"
#include "server_connection.h"
#include "server_response.h"

namespace flame {
namespace net {
namespace fastcgi {

#define CACULATE_PADDING(size) (size) % 8 == 0 ? 0 : 8 - (size) % 8;
	
void server_response::init(server_connection* conn) {
	conn_ = conn;
	conn_->refer();
	prop("header",6) = php::array(0);
}
server_response::~server_response() {
	// fastcgi 协议标志：连接保持
	if((conn_->fpp_.flag & FASTCGI_FLAGS_KEEP_CONN) == 0) {
		conn_->close();
	}
}
php::value server_response::set_cookie(php::parameters& params) {
	if(is_head_sent) {
		throw php::exception("header already sent");
		return nullptr;
	}
	php::buffer data;
	
	std::string name = params[0];
	std::memcpy(data.put(name.length()), name.c_str(), name.length());
	if(params.length() > 1) {
		php::string val = params[1].to_string();
		val = php::url_encode(val.c_str(), val.length());
		data.add('=');
		std::memcpy(data.put(val.length()), val.c_str(), val.length());
	}else{
		std::memcpy(data.put(58), "=deleted; Expires=Thu, 01 Jan 1970 00:00:00 GMT", 58);
	}
	if(params.length() > 2) {
		int64_t ts = params[0].to_long();
		if(ts > 0) {
			struct tm* expire = gmtime((time_t*)&ts);
			strftime(data.put(39), 39, "; Expires=%a, %d %b %Y %H:%M:%S GMT", expire);
		}
	}
	if(params.length() > 3 && params[3].is_string()) {
		php::string& path = params[3];
		sprintf(data.put(7+path.length()), "; Path=%.*s", path.length(), path.c_str());
	}
	if(params.length() > 4 && params[4].is_string()) {
		php::string& domain = params[4];
		sprintf(data.put(9+domain.length()), "; Domain=%.*s", domain.length(), domain.c_str());
	}
	if(params.length() > 5) {
		if(params[5].to_bool()) {
			std::memcpy(data.put(8), "; Secure", 8);
		}
	}
	if(params.length() > 6) {
		if(params[6].to_bool()) {
			std::memcpy(data.put(10), "; HttpOnly", 10);
		}
	}
	cookie_[name] = std::move(data);
	return nullptr;
}
php::value server_response::write_header(php::parameters& params) {
	if(is_head_sent || is_body_sent) {
		throw php::exception("header already sent");
	}
	if(params.length() >= 1) {
		prop("status") = static_cast<int>(params[0]);
	}
	buffer_header();
	is_head_sent = true;

	return write_buffer(params);
}
php::value server_response::write(php::parameters& params) {
	if(is_body_sent) {
		throw php::exception("body already ended");
	}
	if(!is_head_sent) {
		buffer_header();
		is_head_sent = true;
	}
	// 若实际传递的 data 大于可容纳的 body 最大值 64k，需要截断若干次发送 buffer_body 发送
	php::string data = params[0].to_string();
	if(data.length() > 64 * 1024) {
		throw php::exception("write single buffer larger than 64kb is not supported");
	}
	buffer_body(data.data(), data.length());

	return write_buffer(params);
}
php::value server_response::end(php::parameters& params) {
	if(is_body_sent) {
		throw php::exception("body already ended");
	}
	is_body_sent = true;
	if(!is_head_sent) {
		buffer_header();
		is_head_sent = true;
	}
	if(params.length() >= 1) {
		// TODO 若实际传递的 data 大于可容纳的 body 最大值 64k，需要截断若干次发送 buffer_body 发送
		php::string data = params[0].to_string();
		buffer_body(data.data(), data.length());
	}
	buffer_ending();

	return write_buffer(params);
}
void server_response::buffer_header() {
	// 预留头部
	char* head = buffer_.put(sizeof(header_));
	// STATUS_CODE STATUS_TEXT\r\n
	int          status_code = prop("status");
	std::string& status_text = flame::net::http::status_mapper[status_code];
	sprintf(
		buffer_.put(14 + status_text.length()),
		"Status: %03d %.*s\r\n", // fastcgi 返回方式与正常 HTTP 情况不同
		status_code, status_text.length(), status_text.c_str());
	// KEY: VALUE\r\n
	php::array &header = prop("header");
	for(auto i=header.begin(); i!=header.end(); ++i) {
		php::string& key = i->first;
		php::string  val = i->second.to_string();
		sprintf(buffer_.put(key.length() + val.length() + 4),
			"%.*s: %.*s\r\n", key.length(), key.data(),
			val.length(), val.data());
	}
	// Set-Cookie: .....
	for(auto i=cookie_.begin(); i!=cookie_.end(); ++i) {
		sprintf(buffer_.put(14 + i->second.length()),
			"Set-Cookie: %.*s\r\n", i->second.length(), i->second.c_str());
	}
	buffer_.add('\r');
	buffer_.add('\n');
	
	// 根据长度填充头部
	header_.version        = FASTCGI_VERSION;
	header_.type           = FASTCGI_TYPE_STDOUT;
	// !!! 解析过程没有反转，这里也不需要
	header_.request_id     = conn_->fpp_.request_id;
	unsigned short length  = buffer_.size() - sizeof(header_);
	// 注意字节序调整
	header_.content_length = (length & 0x00ff) << 8 | (length & 0xff00) >> 8;
	header_.padding_length = CACULATE_PADDING(length);
	header_.reserved       = 0;
	std::memcpy(head, &header_, sizeof(header_));
	// padding
	if(header_.padding_length > 0) {
		std::memset(buffer_.put(header_.padding_length), 0, header_.padding_length);
	}
}
void server_response::buffer_body(const char* data, unsigned short size) {
	// 根据长度填充头部
	header_.version        = FASTCGI_VERSION;
	header_.type           = FASTCGI_TYPE_STDOUT;
	// !!! 解析过程没有反转，这里也不需要
	header_.request_id     = conn_->fpp_.request_id;
	// 字节序调整
	header_.content_length = (size & 0x00ff) << 8 | (size & 0xff00) >> 8;
	header_.padding_length = CACULATE_PADDING(size);
	header_.reserved       = 0;
	// 头部
	std::memcpy(buffer_.put(sizeof(header_)), &header_, sizeof(header_));
	// 内容
	std::memcpy(buffer_.put(size), data, size);
	// 填充
	if(header_.padding_length > 0) {
		std::memset(buffer_.put(header_.padding_length), 0, header_.padding_length);
	}
}
void server_response::buffer_ending() {
	// 根据长度填充头部
	header_.version        = FASTCGI_VERSION;
	header_.type           = FASTCGI_TYPE_STDOUT;
	// !!! 解析过程没有反转，这里也不需要
	header_.request_id     = conn_->fpp_.request_id;
	header_.content_length = 0;
	header_.padding_length = 0;
	header_.reserved       = 0;
	// 头部
	std::memcpy(buffer_.put(sizeof(header_)), &header_, sizeof(header_));
	// 无内容 无填充
	header_.type = FASTCGI_TYPE_END_REQUEST;
	// length = 8 字节序调整
	header_.content_length = 0x0800;
	// 头部
	std::memcpy(buffer_.put(sizeof(header_)), &header_, sizeof(header_));
	// 内容
	std::memset(buffer_.put(8), 0, 8);
	// 无填充
}
php::value server_response::write_buffer(php::parameters& params) {
	if(conn_->write(std::move(buffer_), coroutine::current)) return flame::async(this);
	if(params.length() > 0 && !params[params.length()-1].is_true()) {
		log::default_logger->write("(WARN) write failed: connection already closed");
	}
	return php::BOOL_NO;
}

}
}
}
