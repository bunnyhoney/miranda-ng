/*

Facebook plugin for Miranda Instant Messenger
_____________________________________________

Copyright � 2009-11 Michal Zelinka, 2011-16 Robert P�sel

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/

#include "stdafx.h"

void facebook_client::client_notify(TCHAR* message)
{
	parent->NotifyEvent(parent->m_tszUserName, message, NULL, FACEBOOK_EVENT_CLIENT);
}

http::response facebook_client::flap(RequestType request_type, std::string *post_data, std::string *get_data)
{
	http::response resp;

	if (parent->isOffline()) {
		resp.code = HTTP_CODE_FAKE_OFFLINE;
		return resp;
	}

	// Prepare the request
	NETLIBHTTPREQUEST nlhr = { sizeof(NETLIBHTTPREQUEST) };

	std::string server = choose_server(request_type);

	// Set request URL
	std::string url = HTTP_PROTO_SECURE + server + choose_action(request_type, get_data);
	if (!parent->m_locale.empty())
		url += "&locale=" + parent->m_locale;
	
	nlhr.szUrl = (char*)url.c_str();

	// Set timeout (bigger for channel request)
	switch (request_type) {
	case REQUEST_MESSAGES_RECEIVE:
		nlhr.timeout = 1000 * 65;
		break;

	default:
		nlhr.timeout = 1000 * 20;
		break;
	}

	// Set request type (GET/POST) and eventually also POST data
	if (post_data != NULL) {
		nlhr.requestType = REQUEST_POST;
		nlhr.pData = (char*)(*post_data).c_str();
		nlhr.dataLength = (int)post_data->length();
	} else {
		nlhr.requestType = REQUEST_GET;
	}

	// Set headers - it depends on requestType so it must be after setting that
	nlhr.headers = get_request_headers(nlhr.requestType, &nlhr.headersCount);	

	// Set flags
	nlhr.flags = NLHRF_HTTP11 | NLHRF_SSL;

	if (server == FACEBOOK_SERVER_MBASIC || server == FACEBOOK_SERVER_MOBILE) {
		nlhr.flags |= NLHRF_REDIRECT;
	}

#ifdef _DEBUG 
	nlhr.flags |= NLHRF_DUMPASTEXT;
#else
	nlhr.flags |= NLHRF_NODUMP;
#endif

	// Set persistent connection (or not)
	switch (request_type) {
	case REQUEST_LOGIN:
		nlhr.nlc = NULL;
		break;

	case REQUEST_MESSAGES_RECEIVE:
		nlhr.nlc = hMsgCon;
		nlhr.flags |= NLHRF_PERSISTENT;
		break;

	default:
		WaitForSingleObject(fcb_conn_lock_, INFINITE);
		nlhr.nlc = hFcbCon;
		nlhr.flags |= NLHRF_PERSISTENT;
		break;
	}

	parent->debugLogA("@@@ Sending request to '%s'", nlhr.szUrl);

	// Send the request	
	NETLIBHTTPREQUEST *pnlhr = (NETLIBHTTPREQUEST*)CallService(MS_NETLIB_HTTPTRANSACTION, (WPARAM)handle_, (LPARAM)&nlhr);	

	mir_free(nlhr.headers[3].szValue);
	mir_free(nlhr.headers);

	// Remember the persistent connection handle (or not)
	switch (request_type) {
	case REQUEST_LOGIN:
	case REQUEST_SETUP_MACHINE:
		break;

	case REQUEST_MESSAGES_RECEIVE:
		hMsgCon = pnlhr ? pnlhr->nlc : NULL;
		break;

	default:
		ReleaseMutex(fcb_conn_lock_);
		hFcbCon = pnlhr ? pnlhr->nlc : NULL;
		break;
	}

	// Check and copy response data
	if (pnlhr != NULL) {
		parent->debugLogA("@@@ Got response with code %d", pnlhr->resultCode);
		store_headers(&resp, pnlhr->headers, pnlhr->headersCount);
		resp.code = pnlhr->resultCode;
		resp.data = pnlhr->pData ? pnlhr->pData : "";

		CallService(MS_NETLIB_FREEHTTPREQUESTSTRUCT, 0, (LPARAM)pnlhr);
	} else {
		parent->debugLogA("!!! No response from server (time-out)");
		resp.code = HTTP_CODE_FAKE_DISCONNECTED;
		// Better to have something set explicitely as this value is compaired in all communication requests
	}

	// Get Facebook's error message
	if (resp.code == HTTP_CODE_OK) {
		std::string::size_type pos = resp.data.find("\"error\":");
		if (pos != std::string::npos) {
			pos += 8;
			int error_num = atoi(resp.data.substr(pos, resp.data.find(",", pos) - pos).c_str());
			if (error_num != 0) {
				std::string error;

				pos = resp.data.find("\"errorDescription\":\"", pos);
				if (pos != std::string::npos) {
					pos += 20;

					std::string::size_type pos2 = resp.data.find("\",\"", pos);
					if (pos2 == std::string::npos) {
						pos2 = resp.data.find("\"", pos);
					}

					error = resp.data.substr(pos, pos2 - pos);
					error = utils::text::trim(utils::text::html_entities_decode(utils::text::remove_html(utils::text::slashu_to_utf8(error))));
					error = ptrA(mir_utf8decodeA(error.c_str()));
				}

				std::string title;
				pos = resp.data.find("\"errorSummary\":\"", pos);
				if (pos != std::string::npos) {
					pos += 16;
					title = resp.data.substr(pos, resp.data.find("\"", pos) - pos);
					title = utils::text::trim(utils::text::html_entities_decode(utils::text::remove_html(utils::text::slashu_to_utf8(title))));
					title = ptrA(mir_utf8decodeA(title.c_str()));
				}

				bool silent = resp.data.find("\"silentError\":1") != std::string::npos;

				resp.error_number = error_num;
				resp.error_text = error;
				resp.error_title = title;
				resp.code = HTTP_CODE_FAKE_ERROR;

				parent->debugLogA("!!! Received Facebook error: %d -- %s", error_num, error.c_str());
				if (notify_errors(request_type) && !silent)
					client_notify(_A2T(error.c_str()));
			}
		}
	}

	return resp;
}

bool facebook_client::handle_entry(const std::string &method)
{
	parent->debugLogA(" >> Entering %s()", method.c_str());
	return true;
}

bool facebook_client::handle_success(const std::string &method)
{
	parent->debugLogA(" << Quitting %s()", method.c_str());
	reset_error();
	return true;
}

bool facebook_client::handle_error(const std::string &method, int action)
{
	increment_error();
	parent->debugLogA("!!! %s(): Something with Facebook went wrong", method.c_str());

	bool result = (error_count_ <= (UINT)parent->getByte(FACEBOOK_KEY_TIMEOUTS_LIMIT, FACEBOOK_TIMEOUTS_LIMIT));
	if (action == FORCE_DISCONNECT || action == FORCE_QUIT)
		result = false;

	if (!result) {
		reset_error();

		if (action != FORCE_QUIT)
			parent->SetStatus(ID_STATUS_OFFLINE);
	}

	return result;
}

//////////////////////////////////////////////////////////////////////////////

std::string facebook_client::choose_server(RequestType request_type)
{
	switch (request_type)
	{
	case REQUEST_LOGIN:
		return FACEBOOK_SERVER_LOGIN;

	case REQUEST_MESSAGES_RECEIVE:
	case REQUEST_ACTIVE_PING:
	{
		std::string server = FACEBOOK_SERVER_CHAT;
		utils::text::replace_first(&server, "%s", this->chat_conn_num_.empty() ? "0" : this->chat_conn_num_);
		utils::text::replace_first(&server, "%s", this->chat_channel_host_);
		return server;
	}

	case REQUEST_HOME:
	case REQUEST_DTSG:
		return FACEBOOK_SERVER_MOBILE;

	case REQUEST_LOAD_FRIENDSHIPS:
	case REQUEST_SEARCH:
	case REQUEST_USER_INFO_MOBILE:
		return this->mbasicWorks ? FACEBOOK_SERVER_MBASIC : FACEBOOK_SERVER_MOBILE;

		//	case REQUEST_LOGOUT:
		//	case REQUEST_BUDDY_LIST:
		//	case REQUEST_USER_INFO:
		//	case REQUEST_USER_INFO_ALL:
		//	case REQUEST_FEEDS:
		//	case REQUEST_PAGES:
		//	case REQUEST_NOTIFICATIONS:
		//	case REQUEST_RECONNECT:
		//	case REQUEST_POST_STATUS:
		//	case REQUEST_IDENTITY_SWITCH:
		//	case REQUEST_CAPTCHA_REFRESH:
		//	case REQUEST_LINK_SCRAPER:
		//	case REQUEST_MESSAGES_SEND:
		//	case REQUEST_THREAD_INFO:
		//	case REQUEST_THREAD_SYNC:
		//	case REQUEST_VISIBILITY:
		//	case REQUEST_POKE:
		//	case REQUEST_MARK_READ:
		//	case REQUEST_NOTIFICATIONS_READ:
		//	case REQUEST_TYPING_SEND:
		//	case REQUEST_SETUP_MACHINE:
		//  case REQUEST_DELETE_FRIEND:
		//	case REQUEST_ADD_FRIEND:
		//	case REQUEST_CANCEL_FRIENDSHIP:
		//	case REQUEST_FRIENDSHIP:
		//	case REQUEST_UNREAD_THREADS:
		//	case REQUEST_ON_THIS_DAY:
		//	case REQUEST_LOGIN_SMS:
	default:
		return FACEBOOK_SERVER_REGULAR;
	}
}

std::string facebook_client::choose_action(RequestType request_type, std::string *get_data)
{
	switch (request_type)
	{
	case REQUEST_LOGIN:
	{
		std::string action = "/login.php?login_attempt=1";
		if (get_data != NULL) {
			action += *get_data;
		}
		return action;
	}

	case REQUEST_SETUP_MACHINE:
		return "/checkpoint/?next";

	case REQUEST_LOGOUT:
		return "/logout.php?";

	case REQUEST_HOME:
		return "/profile.php?v=info";

	case REQUEST_DTSG:
		return "/editprofile.php?edit=current_city&type=basic";

	case REQUEST_BUDDY_LIST:
		return "/ajax/chat/buddy_list.php?__a=1";

	case REQUEST_USER_INFO:
		return "/ajax/chat/user_info.php?__a=1";

	case REQUEST_USER_INFO_ALL:
		return "/ajax/chat/user_info_all.php?__a=1&viewer=" + self_.user_id;

	case REQUEST_USER_INFO_MOBILE:
	{
		std::string action = "/%sv=info";
		if (get_data != NULL) {
			utils::text::replace_all(&action, "%s", *get_data);
		}
		return action;
	}

	case REQUEST_LOAD_FRIENDSHIPS:
	{
		return "/friends/center/requests/?";
	}

	case REQUEST_SEARCH:
	{
		std::string action = "/search/?search=people&query=";
		if (get_data != NULL) {
			action += *get_data;
		}
		return action;
	}

	case REQUEST_UNREAD_THREADS:
	{
		return "/ajax/mercury/unread_threads.php?__a=1";
	}

	case REQUEST_DELETE_FRIEND:
	{
		std::string action = "/ajax/profile/removefriendconfirm.php?__a=1";
		if (get_data != NULL) {
			action += *get_data;
		}
		return action;
	}

	case REQUEST_ADD_FRIEND:
	{
		return "/ajax/add_friend/action.php?__a=1";
	}

	case REQUEST_CANCEL_FRIENDSHIP:
	{
		return "/ajax/friends/requests/cancel.php?__a=1";
	}

	case REQUEST_FRIENDSHIP:
	{
		return "/requests/friends/ajax/?__a=1";
	}

	case REQUEST_FEEDS:
	{
		std::string action = "/ajax/home/generic.php?" + get_newsfeed_type();
		action += "&__user=" + self_.user_id + "&__a=1";

		/*std::string newest = utils::conversion::to_string((void*)&this->last_feeds_update_, UTILS_CONV_TIME_T);
		utils::text::replace_first(&action, "%s", newest);
		utils::text::replace_first(&action, "%s", self_.user_id);*/
		return action;
	}

	case REQUEST_PAGES:
	{
		return "/bookmarks/pages?";
	}

	case REQUEST_NOTIFICATIONS:
	{
		return "/ajax/notifications/client/get.php?__a=1";
	}

	case REQUEST_RECONNECT:
	{
		std::string action = "/ajax/presence/reconnect.php?__a=1&reason=%s&fb_dtsg=%s&__user=%s";

		if (this->chat_reconnect_reason_.empty())
			this->chat_reconnect_reason_ = "6";

		utils::text::replace_first(&action, "%s", this->chat_reconnect_reason_);
		utils::text::replace_first(&action, "%s", this->dtsg_);
		utils::text::replace_first(&action, "%s", this->self_.user_id);

		action += "&__dyn=" + __dyn();
		action += "&__req=" + __req();
		action += "&__rev=" + __rev();

		return action;
	}

	case REQUEST_POST_STATUS:
		return "/ajax/updatestatus.php?__a=1";

	case REQUEST_IDENTITY_SWITCH:
		return "/identity_switch.php?__a=1";

	case REQUEST_CAPTCHA_REFRESH:
	{
		std::string action = "/captcha/refresh_ajax.php?__a=1";
		if (get_data != NULL) {
			action += "&" + (*get_data);
		}
		return action;
	}

	case REQUEST_LINK_SCRAPER:
	{
		std::string action = "/ajax/composerx/attachment/link/scraper/?__a=1&composerurihash=2&scrape_url=";
		if (get_data != NULL) {
			action += utils::url::encode(*get_data);
		}
		return action;
	}

	case REQUEST_MESSAGES_SEND:
		return "/ajax/mercury/send_messages.php?__a=1";

	case REQUEST_THREAD_INFO:
		return "/ajax/mercury/thread_info.php?__a=1";

	case REQUEST_THREAD_SYNC:
		return "/ajax/mercury/thread_sync.php?__a=1";

	case REQUEST_MESSAGES_RECEIVE:
	case REQUEST_ACTIVE_PING:
	{
		bool isPing = (request_type == REQUEST_ACTIVE_PING);

		std::string action = (isPing ? "/active_ping" : "/pull");
		action += "?channel=" + (this->chat_channel_.empty() ? "p_" + self_.user_id : this->chat_channel_);		
		if (!isPing)
			action += "&seq=" + (this->chat_sequence_num_.empty() ? "0" : this->chat_sequence_num_);
		action += "&partition=" + (this->chat_channel_partition_.empty() ? "0" : this->chat_channel_partition_);
		action += "&clientid=" + this->chat_clientid_;
		action += "&cb=" + utils::text::rand_string(4, "0123456789abcdefghijklmnopqrstuvwxyz", &this->random_);

		/*
		original cb = return (1048576 * Math.random() | 0).toString(36);
		char buffer[10];
		itoa(((int)(1048576 * (((double)rand()) / (RAND_MAX + 1))) | 0), buffer, 36);
		action += "&cb=" + buffer;
		*/

		int idleSeconds = parent->IdleSeconds();
		if (idleSeconds > 0 && !parent->isInvisible())
			action += "&idle=" + utils::conversion::to_string(&idleSeconds, UTILS_CONV_UNSIGNED_NUMBER);

		if (!isPing) {
			action += "&qp=y"; // TODO: what's this item?
			action += "&pws=fresh"; // TODO: what's this item?
			action += "&isq=487632"; // TODO: what's this item?
			action += "&msgs_recv=" + utils::conversion::to_string(&this->chat_msgs_recv_, UTILS_CONV_UNSIGNED_NUMBER);
			// TODO: sometimes there is &tur=1711 and &qpmade=<some actual timestamp> and &isq=487632
			// action += "&request_batch=1"; // it somehow batches up more responses to one - then response has special "t=batched" type and "batches" array with the data
			// action += "&msgr_region=LLA"; // it was here only for first pull, same as request_batch
		}

		action += "&cap=8"; // TODO: what's this item? Sometimes it's 0, sometimes 8
		action += "&uid=" + self_.user_id;
		action += "&viewer_uid=" + self_.user_id;

		if (!this->chat_sticky_num_.empty() && !this->chat_sticky_pool_.empty()) {
			action += "&sticky_token=" + this->chat_sticky_num_;
			action += "&sticky_pool=" + this->chat_sticky_pool_;
		}

		if (!isPing && !this->chat_traceid_.empty())
			action += "&traceid=" + this->chat_traceid_;

		if (parent->isInvisible())
			action += "&state=offline";
		else if (isPing || idleSeconds < 60)
			action += "&state=active";

		return action;
	}

	case REQUEST_VISIBILITY:
		return "/ajax/chat/privacy/visibility.php?__a=1";

	case REQUEST_POKE:
		return "/pokes/dialog/?__a=1";

	case REQUEST_MARK_READ:
		return "/ajax/mercury/change_read_status.php?__a=1";

	case REQUEST_NOTIFICATIONS_READ:
	{
		std::string action = "/ajax/notifications/mark_read.php?__a=1";
		if (get_data != NULL) {
			action += "&" + (*get_data);
		}
		return action;
	}

	case REQUEST_TYPING_SEND:
		return "/ajax/messaging/typ.php?__a=1";

	case REQUEST_ON_THIS_DAY:
	{
		std::string action = "/onthisday/story/query/?__a=1";
		if (get_data != NULL) {
			action += "&" + (*get_data);
		}
		return action;
	}

	case REQUEST_LOGIN_SMS:
	{
		return "/ajax/login/approvals/send_sms?dpr=1";
	}

	default:
		return "/?_fb_noscript=1";
	}
}

bool facebook_client::notify_errors(RequestType request_type)
{
	switch (request_type)
	{
	case REQUEST_BUDDY_LIST:
	case REQUEST_MESSAGES_SEND:
		return false;

	default:
		return true;
	}
}

NETLIBHTTPHEADER *facebook_client::get_request_headers(int request_type, int *headers_count)
{
	if (request_type == REQUEST_POST)
		*headers_count = 5;
	else
		*headers_count = 4;

	NETLIBHTTPHEADER *headers = (NETLIBHTTPHEADER*)mir_calloc(sizeof(NETLIBHTTPHEADER)*(*headers_count));

	if (request_type == REQUEST_POST) {
		headers[4].szName = "Content-Type";
		headers[4].szValue = "application/x-www-form-urlencoded; charset=utf-8";
	}

	headers[3].szName = "Cookie";
	headers[3].szValue = load_cookies();
	headers[2].szName = "User-Agent";
	headers[2].szValue = (char *)g_strUserAgent.c_str();
	headers[1].szName = "Accept";
	headers[1].szValue = "*/*";
	headers[0].szName = "Accept-Language";
	headers[0].szValue = "en,en-US;q=0.9";

	return headers;
}

std::string facebook_client::get_newsfeed_type()
{
	BYTE feed_type = parent->getByte(FACEBOOK_KEY_FEED_TYPE, 0);
	if (feed_type >= _countof(feed_types))
		feed_type = 0;

	std::string ret = "sk=";
	ret += feed_types[feed_type].id;
	ret += "&key=";
	ret += (feed_type < 2 ? "nf" : feed_types[feed_type].id);
	return ret;
}

std::string facebook_client::get_server_type()
{
	BYTE server_type = parent->getByte(FACEBOOK_KEY_SERVER_TYPE, 0);
	if (server_type >= _countof(server_types))
		server_type = 0;
	return server_types[server_type].id;
}

std::string facebook_client::get_privacy_type()
{
	BYTE privacy_type = parent->getByte(FACEBOOK_KEY_PRIVACY_TYPE, 0);
	if (privacy_type >= _countof(privacy_types))
		privacy_type = 0;
	return privacy_types[privacy_type].id;
}


char* facebook_client::load_cookies()
{
	ScopedLock s(cookies_lock_);

	std::string cookieString;

	if (!cookies.empty()) {
		for (std::map< std::string, std::string >::iterator iter = cookies.begin(); iter != cookies.end(); ++iter)
		{
			cookieString.append(iter->first);
			cookieString.append(1, '=');
			cookieString.append(iter->second);
			cookieString.append(1, ';');
		}
	}

	return mir_strdup(cookieString.c_str());
}

void facebook_client::store_headers(http::response* resp, NETLIBHTTPHEADER* headers, int headersCount)
{
	ScopedLock c(cookies_lock_);

	for (int i = 0; i < headersCount; i++) {
		std::string header_name = headers[i].szName;
		std::string header_value = headers[i].szValue;

		if (header_name == "Set-Cookie") {
			std::string::size_type pos = header_value.find("=");
			std::string cookie_name = header_value.substr(0, pos);
			std::string cookie_value = header_value.substr(pos + 1, header_value.find(";", pos) - pos - 1);

			if (cookie_value == "deleted")
				cookies.erase(cookie_name);
			else
				cookies[cookie_name] = cookie_value;
		} else {
			resp->headers[header_name] = header_value;
		}
	}
}

void facebook_client::clear_cookies()
{
	ScopedLock s(cookies_lock_);

	if (!cookies.empty())
		cookies.clear();
}

void facebook_client::clear_notifications()
{
	ScopedLock s(notifications_lock_);

	for (auto it = notifications.begin(); it != notifications.end();) {
		if (it->second->hWndPopup != NULL)
			PUDeletePopup(it->second->hWndPopup); // close popup

		delete it->second;
		it = notifications.erase(it);
	}

	notifications.clear();
}

void facebook_client::clear_chatrooms()
{
	for (auto it = chat_rooms.begin(); it != chat_rooms.end();) {
		delete it->second;
		it = chat_rooms.erase(it);
	}
	chat_rooms.clear();
}

/**
 * Clears readers info for all contacts from readers list and db
 */
void facebook_client::clear_readers()
{
	for (std::map<MCONTACT, time_t>::iterator it = readers.begin(); it != readers.end();) {
		if (parent->isChatRoom(it->first))
			parent->delSetting(it->first, FACEBOOK_KEY_MESSAGE_READERS);

		parent->delSetting(it->first, FACEBOOK_KEY_MESSAGE_READ);
		it = readers.erase(it);
	}
	readers.clear();
}

/**
 * Inserts info to readers list, db and writes to statusbar
 */
void facebook_client::insert_reader(MCONTACT hContact, time_t timestamp, const std::tstring &reader)
{
	if (parent->isChatRoom(hContact)) {
		std::tstring treaders;

		// Load old readers
		ptrT told(parent->getTStringA(hContact, FACEBOOK_KEY_MESSAGE_READERS));
		if (told)
			treaders = std::tstring(told) + _T(", ");

		// Append new reader name and remember them
		treaders += utils::text::prepare_name(reader, true);
		parent->setTString(hContact, FACEBOOK_KEY_MESSAGE_READERS, treaders.c_str());
	}

	parent->setDword(hContact, FACEBOOK_KEY_MESSAGE_READ, timestamp);
	readers.insert(std::make_pair(hContact, timestamp));
	parent->MessageRead(hContact);
	if (ServiceExists(MS_MESSAGESTATE_UPDATE)) 
	{
		MessageReadData data(timestamp, MRD_TYPE_READTIME); 
		CallService(MS_MESSAGESTATE_UPDATE, hContact, (LPARAM)&data);
	}
}

/**
 * Removes info from readers list, db and clears statusbar
 */
void facebook_client::erase_reader(MCONTACT hContact)
{
	if (parent->isChatRoom(hContact)) {
		parent->delSetting(hContact, FACEBOOK_KEY_MESSAGE_READERS);
	}
	
	parent->delSetting(hContact, FACEBOOK_KEY_MESSAGE_READ);

	readers.erase(hContact);
	CallService(MS_MSG_SETSTATUSTEXT, (WPARAM)hContact);
}

void loginError(FacebookProto *proto, std::string error_str) {
	error_str = utils::text::trim(
		utils::text::html_entities_decode(
		utils::text::remove_html(
		utils::text::edit_html(error_str))));

	proto->debugLogA("!!! Login error: %s", !error_str.empty() ? error_str.c_str() : "Unknown error");

	TCHAR buf[200];
	mir_sntprintf(buf, TranslateT("Login error: %s"),
		(error_str.empty()) ? TranslateT("Unknown error") : ptrT(mir_utf8decodeT(error_str.c_str())));
	proto->facy.client_notify(buf);
}

void parseJsCookies(const std::string &search, const std::string &data, std::map<std::string, std::string> &cookies) {
	std::string::size_type pos = 0;
	while ((pos = data.find(search, pos)) != std::string::npos) {
		pos += search.length();

		std::string::size_type pos2 = data.find("\",\"", pos);
		if (pos2 == std::string::npos)
			continue;

		std::string name = utils::url::encode(data.substr(pos, pos2 - pos));

		pos = pos2 + 3;
		pos2 = data.find("\"", pos);
		if (pos2 == std::string::npos)
			continue;

		std::string value = data.substr(pos, pos2 - pos);
		cookies[name] = utils::url::encode(utils::text::html_entities_decode(value));
	}
}

bool facebook_client::login(const char *username, const char *password)
{
	handle_entry("login");

	username_ = username;
	password_ = password;

	// Prepare login data
	std::string data = "persistent=1";
	data += "&email=" + utils::url::encode(username);
	data += "&pass=" + utils::url::encode(password);

	std::string get_data = "";

	if (cookies.empty()) {
		// Set device ID
		ptrA device(parent->getStringA(FACEBOOK_KEY_DEVICE_ID));
		if (device != NULL)
			cookies["datr"] = device;

		// Get initial cookies
		http::response resp = flap(REQUEST_LOGIN);

		// Also parse cookies set by JavaScript (more variant exists in time, so check all known now)
		parseJsCookies("[\"DeferredCookie\",\"addToQueue\",[],[\"", resp.data, cookies);
		parseJsCookies("[\"Cookie\",\"setIfFirstPartyContext\",[],[\"", resp.data, cookies);

		// Parse hidden inputs and other data
		std::string form = utils::text::source_get_value(&resp.data, 2, "<form", "</form>");
		utils::text::replace_all(&form, "\\\"", "\"");

		data += "&" + utils::text::source_get_form_data(&form, true);
		get_data += "&" + utils::text::source_get_value(&form, 2, "login.php?login_attempt=1&amp;", "\"");
	}

	data += "&lgndim=eyJ3IjoxOTIwLCJoIjoxMDgwLCJhdyI6MTgzNCwiYWgiOjEwODAsImMiOjMyfQ=="; // means base64 encoded: {"w":1920,"h":1080,"aw":1834,"ah":1080,"c":32}

	// Send validation
	http::response resp = flap(REQUEST_LOGIN, &data, &get_data);

	// Save Device ID
	if (!cookies["datr"].empty())
		parent->setString(FACEBOOK_KEY_DEVICE_ID, cookies["datr"].c_str());

	if (resp.code == HTTP_CODE_FOUND && resp.headers.find("Location") != resp.headers.end())
	{
		std::string location = resp.headers["Location"];

		// Check for invalid requests
		if (location.find("invalid_request.php") != std::string::npos) {
			client_notify(TranslateT("Login error: Invalid request."));
			parent->debugLogA("!!! Login error: Invalid request.");
			return handle_error("login", FORCE_QUIT);
		}

		// Check whether login checks are required
		if (location.find("/checkpoint/") != std::string::npos) {
			resp = flap(REQUEST_SETUP_MACHINE, NULL, NULL);

			if (resp.data.find("login_approvals_no_phones") != std::string::npos) {
				// Code approval - but no phones in account
				loginError(parent, utils::text::source_get_value(&resp.data, 4, "login_approvals_no_phones", "<div", ">", "</div>"));
				return handle_error("login", FORCE_QUIT);
			}

			if (resp.data.find("name=\"submit[Continue]\"") != std::string::npos) {
				std::string inner_data;

				int attempt = 0;
				// Check if we need to put approval code (aka "two-factor auth")
				while (resp.data.find("id=\"approvals_code\"") != std::string::npos) {
					parent->debugLogA("    Login info: Approval code required.");

					std::string fb_dtsg = utils::url::encode(utils::text::source_get_value(&resp.data, 3, "name=\"fb_dtsg\"", "value=\"", "\""));

					CFacebookGuardDialog guardDialog(parent, fb_dtsg.c_str());
					if (guardDialog.DoModal() != DIALOG_RESULT_OK) {
						parent->SetStatus(ID_STATUS_OFFLINE);
						return false;
					}

					// We need verification code from user (he can get it via Facebook application on phone or by requesting code via SMS)
					std::string givenCode = guardDialog.GetCode();

					inner_data = "submit[Continue]=Continue";
					inner_data += "&nh=" + utils::text::source_get_value(&resp.data, 3, "name=\"nh\"", "value=\"", "\"");
					inner_data += "&fb_dtsg=" + fb_dtsg;
					inner_data += "&approvals_code=" + givenCode;
					resp = flap(REQUEST_SETUP_MACHINE, &inner_data);

					if (resp.data.find("id=\"approvals_code\"") != std::string::npos) {
						// We get no error message if we put wrong code. Facebook just shows same form again.
						if (++attempt >= 3) {
							client_notify(TranslateT("You entered too many invalid verification codes. Plugin will disconnect."));
							parent->debugLogA("!!! Login error: Too many invalid attempts to verification code.");
							return handle_error("login", FORCE_QUIT);
						}
						else {
							client_notify(TranslateT("You entered wrong verification code. Try it again."));
						}
					}
					else {
						// After successfull verification is showed different page - classic form to save device (as handled at the bottom)
						break;
					}
				}
				
				// Check if we need to approve also last unapproved device
				if (resp.data.find("name=\"name_action_selected\"") == std::string::npos) {
					// 1) Continue
					inner_data = "submit[Continue]=Continue";
					inner_data += "&nh=" + utils::text::source_get_value(&resp.data, 3, "name=\"nh\"", "value=\"", "\"");
					inner_data += "&fb_dtsg=" + utils::url::encode(utils::text::source_get_value(&resp.data, 3, "name=\"fb_dtsg\"", "value=\"", "\""));
					resp = flap(REQUEST_SETUP_MACHINE, &inner_data);

					// In this step might be needed identity confirmation
					if (resp.data.find("name=\"birthday_captcha_") != std::string::npos) {
						// Account is locked and needs identity confirmation
						client_notify(TranslateT("Login error: Your account is temporarily locked. You need to confirm this device from web browser."));
						parent->debugLogA("!!! Login error: Birthday confirmation.");
						return handle_error("login", FORCE_QUIT);
					}

					// 2) Approve last unknown login
					// inner_data = "submit[I%20don't%20recognize]=I%20don't%20recognize"; // Don't recognize - this will force to change account password
					inner_data = "submit[This%20is%20Okay]=This%20is%20Okay"; // Recognize
					inner_data += "&submit[This is Okay]=This is Okay"; // I don't know whether it's with classic spaces now or not
					inner_data += "&nh=" + utils::text::source_get_value(&resp.data, 3, "name=\"nh\"", "value=\"", "\"");
					inner_data += "&fb_dtsg=" + utils::url::encode(utils::text::source_get_value(&resp.data, 3, "name=\"fb_dtsg\"", "value=\"", "\""));
					resp = flap(REQUEST_SETUP_MACHINE, &inner_data);

					// 3) Save last device
					inner_data = "submit[Continue]=Continue";
					inner_data += "&nh=" + utils::text::source_get_value(&resp.data, 3, "name=\"nh\"", "value=\"", "\"");
					inner_data += "&fb_dtsg=" + utils::url::encode(utils::text::source_get_value(&resp.data, 3, "name=\"fb_dtsg\"", "value=\"", "\""));
					inner_data += "&name_action_selected=save_device"; // Save device - or "dont_save"
					resp = flap(REQUEST_SETUP_MACHINE, &inner_data);
				}
				
				// Save this actual device
				inner_data = "submit[Continue]=Continue";
				inner_data += "&nh=" + utils::text::source_get_value(&resp.data, 3, "name=\"nh\"", "value=\"", "\"");
				inner_data += "&fb_dtsg=" + utils::url::encode(utils::text::source_get_value(&resp.data, 3, "name=\"fb_dtsg\"", "value=\"", "\""));
				inner_data += "&name_action_selected=save_device"; // Save device - or "dont_save"
				resp = flap(REQUEST_SETUP_MACHINE, &inner_data);
			}
			else if (resp.data.find("name=\"submit[Get Started]\"") != std::string::npos) {
				if (!parent->getBool(FACEBOOK_KEY_TRIED_DELETING_DEVICE_ID)) {
					// Try to remove DeviceID and login again
					cookies["datr"] = "";
					parent->delSetting(FACEBOOK_KEY_DEVICE_ID);
					parent->setByte(FACEBOOK_KEY_TRIED_DELETING_DEVICE_ID, 1);
					return login(username, password);
				} else {
					// Reset flag
					parent->delSetting(FACEBOOK_KEY_TRIED_DELETING_DEVICE_ID);
					// Facebook things that computer was infected by malware and needs cleaning
					client_notify(TranslateT("Login error: Facebook thinks your computer is infected. Solve it by logging in via 'private browsing' mode of your web browser and run their antivirus check."));
					parent->debugLogA("!!! Login error: Facebook requires computer scan.");
					return handle_error("login", FORCE_QUIT);
				}
			}
		}
	}

	switch (resp.code)
	{
	case HTTP_CODE_FAKE_DISCONNECTED:
	{
		// When is error only because timeout, try login once more
		if (handle_error("login"))
			return login(username, password);
		else
			return handle_error("login", FORCE_QUIT);
	}

	case HTTP_CODE_OK: // OK page returned, but that is regular login page we don't want in fact
	{
		// Check whether captcha code is required
		if (resp.data.find("id=\"captcha\"") != std::string::npos) {
			client_notify(TranslateT("Login error: Captcha code is required. You need to confirm this device from web browser."));
			parent->debugLogA("!!! Login error: Captcha code is required.");
			return handle_error("login", FORCE_QUIT);
		}

		// Get and notify error message
		std::string error = utils::text::slashu_to_utf8(utils::text::source_get_value(&resp.data, 3, "[\"LoginFormError\"", "\"__html\":\"", "\"}"));
		if (error.empty())
			error = utils::text::slashu_to_utf8(utils::text::source_get_value(&resp.data, 3, "role=\"alert\"", ">", "</div"));
		if (error.empty())
			error = utils::text::slashu_to_utf8(utils::text::source_get_value(&resp.data, 3, "id=\"globalContainer\"", ">", "</div"));
		if (error.empty())
			error = utils::text::slashu_to_utf8(utils::text::source_get_value(&resp.data, 2, "<strong>", "</strong"));
		loginError(parent, error);
	}
	case HTTP_CODE_FORBIDDEN: // Forbidden
	case HTTP_CODE_NOT_FOUND: // Not Found
	default:
		return handle_error("login", FORCE_QUIT);

	case HTTP_CODE_FOUND: // Found and redirected somewhere
	{
		if (resp.headers.find("Location") != resp.headers.end()) {
			std::string redirectUrl = resp.headers["Location"];
			std::string expectedUrl = HTTP_PROTO_SECURE FACEBOOK_SERVER_REGULAR "/";

			// Remove eventual parameters
			std::string::size_type pos = redirectUrl.rfind("?");
			if (pos != std::string::npos)
				redirectUrl = redirectUrl.substr(0, pos);
			
			if (redirectUrl != expectedUrl) {
				// Unexpected redirect, but we try to ignore it - maybe we were logged in anyway
				parent->debugLogA("!!! Login error: Unexpected redirect: %s (Original: %s) (Expected: %s)", redirectUrl.c_str(), resp.headers["Location"].c_str(), expectedUrl.c_str());
			}
		}

		if (cookies.find("c_user") != cookies.end()) {
			// Probably logged in, everything seems OK
			this->self_.user_id = cookies.find("c_user")->second;
			parent->setString(FACEBOOK_KEY_ID, this->self_.user_id.c_str());
			parent->debugLogA("    Got self user id: %s", this->self_.user_id.c_str());
			return handle_success("login");
		}
		else {
			client_notify(TranslateT("Login error, probably bad login credentials."));
			parent->debugLogA("!!! Login error, probably bad login credentials.");
			return handle_error("login", FORCE_QUIT);
		}
	}
	}
}

bool facebook_client::logout()
{
	handle_entry("logout");

	std::string data = "fb_dtsg=" + this->dtsg_;
	data += "&ref=mb&h=" + this->logout_hash_;

	http::response resp = flap(REQUEST_LOGOUT, &data);

	this->username_.clear();
	this->password_.clear();
	this->self_.user_id.clear();

	switch (resp.code)
	{
	case HTTP_CODE_OK:
	case HTTP_CODE_FOUND:
		return handle_success("logout");

	default:
		return false; // Logout not finished properly, but..okay, who cares :P
	}
}

bool facebook_client::home()
{
	handle_entry("home");

	// get fb_dtsg
	http::response resp = flap(REQUEST_DTSG);

	this->dtsg_ = utils::url::encode(utils::text::source_get_value(&resp.data, 3, "name=\"fb_dtsg\"", "value=\"", "\""));
	{
		// Compute ttstamp from dtsg_
		std::stringstream csrf;
		for (unsigned int i = 0; i < this->dtsg_.length(); i++) {
			csrf << (int)this->dtsg_.at(i);
		}
		this->ttstamp_ = "2" + csrf.str();
	}	

	if (this->dtsg_.empty()) {
		parent->debugLogA("!!! Empty dtsg. Source code:\n%s", resp.data.c_str());
		client_notify(TranslateT("Could not load communication token. You should report this and wait for plugin update."));
		return handle_error("home", FORCE_QUIT);
	} else {
		parent->debugLogA("    Got self dtsg");
	}

	resp = flap(REQUEST_HOME);

	switch (resp.code)
	{
	case HTTP_CODE_OK:
	{
		std::string touchSearch = "{\"id\":" + this->self_.user_id;
		std::string touchData = utils::text::source_get_value(&resp.data, 2, touchSearch.c_str(), "}");

		// Get real name (from touch version)
		if (!touchData.empty())
			this->self_.real_name = utils::text::html_entities_decode(utils::text::slashu_to_utf8(utils::text::source_get_value(&touchData, 2, "\"name\":\"", "\"")));

		// Another attempt to get real name (from mbasic version)
		if (this->self_.real_name.empty())
			this->self_.real_name = utils::text::source_get_value(&resp.data, 4, "id=\"root", "<strong", ">", "</strong>");

		// Try to get name again, if we've got some some weird version of Facebook
		if (this->self_.real_name.empty())
			this->self_.real_name = utils::text::source_get_value(&resp.data, 5, "id=\"root", "</a>", "<div", ">", "</div>");

		// Another attempt to get name
		if (this->self_.real_name.empty())
			this->self_.real_name = utils::text::source_get_value(&resp.data, 5, "id=\"root", "</td>", "<div", ">", "</td>");

		// Get and strip optional nickname
		std::string::size_type pos = this->self_.real_name.find("<span class=\"alternate_name\">");
		if (pos != std::string::npos) {
			this->self_.nick = utils::text::source_get_value(&this->self_.real_name, 2, "<span class=\"alternate_name\">(", ")</span>");
			parent->debugLogA("    Got self nick name: %s", this->self_.nick.c_str());

			this->self_.real_name = this->self_.real_name.substr(0, pos - 1);
		}
		
		// Another attempt to get optional nickname
		if (this->self_.nick.empty())
			this->self_.nick = utils::text::html_entities_decode(utils::text::slashu_to_utf8(utils::text::source_get_value(&resp.data, 3, "class=\\\"alternate_name\\\"", ">(", ")\\u003C\\/")));

		this->self_.real_name = utils::text::remove_html(this->self_.real_name);
		parent->debugLogA("    Got self real name (nickname): %s (%s)", this->self_.real_name.c_str(), this->self_.nick.c_str());
		parent->SaveName(NULL, &this->self_);

		// Get avatar (from touch version)
		if (!touchData.empty())
			this->self_.image_url = utils::text::html_entities_decode(utils::text::slashu_to_utf8(utils::text::source_get_value(&touchData, 2, "\"pic\":\"", "\"")));

		// Another attempt to get avatar(from mbasic version)
		if (this->self_.image_url.empty())
			this->self_.image_url = utils::text::source_get_value(&resp.data, 3, "id=\"root", "<img src=\"", "\"");
		
		// Another attempt to get avatar
		if (this->self_.image_url.empty()) {
			this->self_.image_url = utils::text::source_get_value(&resp.data, 3, "id=\"root", "/photo.php?", "\"");
			
			// Prepare this special url (not direct image url) to be handled correctly in CheckAvatarChange()
			// It must contain "/" at the beginning and also shouldn't contain "?" as parameters after that are stripped
			if (!this->self_.image_url.empty())
				this->self_.image_url = "/" + this->self_.image_url;
		}

		parent->debugLogA("    Got self avatar: %s", this->self_.image_url.c_str());
		parent->CheckAvatarChange(NULL, this->self_.image_url);

		// Get logout hash
		this->logout_hash_ = utils::text::source_get_value2(&resp.data, "/logout.php?h=", "&\"");
		parent->debugLogA("    Got self logout hash: %s", this->logout_hash_.c_str());

		if (this->self_.real_name.empty() || this->self_.image_url.empty() || this->logout_hash_.empty()) {
			parent->debugLogA("!!! Empty nick/avatar/hash. Source code:\n%s", resp.data.c_str());
			client_notify(TranslateT("Could not load all required data. Plugin may still work correctly, but you should report this and wait for plugin update."));
		}

		return handle_success("home");
	}
	case HTTP_CODE_FOUND:
		// Work-around for replica_down, f**king hell what's that?
		parent->debugLogA("!!! REPLICA_DOWN is back in force!");
		return this->home();

	default:
		return handle_error("home", FORCE_QUIT);
	}
}

bool facebook_client::chat_state(bool online)
{
	handle_entry("chat_state");

	std::string data = (online ? "visibility=1" : "visibility=0");
	data += "&window_id=0";
	data += "&fb_dtsg=" + dtsg_;
	data += "&__user=" + self_.user_id;
	data += "&__dyn=" + __dyn();
	data += "&__req=" + __req();
	data += "&ttstamp=" + ttstamp_;
	data += "&__rev=" + __rev();
	http::response resp = flap(REQUEST_VISIBILITY, &data); // NOTE: Request revised 11.2.2016

	if (!resp.error_title.empty())
		return handle_error("chat_state");

	return handle_success("chat_state");
}

bool facebook_client::reconnect()
{
	handle_entry("reconnect");

	// Request reconnect
	http::response resp = flap(REQUEST_RECONNECT);

	switch (resp.code)
	{
	case HTTP_CODE_OK:
	{
		this->chat_channel_ = utils::text::source_get_value(&resp.data, 2, "\"user_channel\":\"", "\"");
		parent->debugLogA("    Got self channel: %s", this->chat_channel_.c_str());

		this->chat_channel_partition_ = utils::text::source_get_value2(&resp.data, "\"partition\":", ",}");
		parent->debugLogA("    Got self channel partition: %s", this->chat_channel_partition_.c_str());

		this->chat_channel_host_ = utils::text::source_get_value(&resp.data, 2, "\"host\":\"", "\"");
		parent->debugLogA("    Got self channel host: %s", this->chat_channel_host_.c_str());

		this->chat_sequence_num_ = utils::text::source_get_value2(&resp.data, "\"seq\":", ",}");
		parent->debugLogA("    Got self sequence number: %s", this->chat_sequence_num_.c_str());

		this->chat_conn_num_ = utils::text::source_get_value2(&resp.data, "\"max_conn\":", ",}");
		parent->debugLogA("    Got self max_conn: %s", this->chat_conn_num_.c_str());

		this->chat_sticky_num_ = utils::text::source_get_value(&resp.data, 2, "\"sticky_token\":\"", "\"");
		parent->debugLogA("    Got self sticky_token: %s", this->chat_sticky_num_.c_str());

		//std::string retry_interval = utils::text::source_get_value2(&resp.data, "\"retry_interval\":", ",}");
		//parent->debugLogA("    Got self retry_interval: %s", retry_interval.c_str());

		//std::string visibility = utils::text::source_get_value2(&resp.data, "\"visibility\":", ",}");
		//parent->debugLogA("    Got self visibility: %s", visibility.c_str());

		// Send activity_ping after each reconnect
		activity_ping();

		return handle_success("reconnect");
	}

	default:
		return handle_error("reconnect", FORCE_DISCONNECT);
	}
}

bool facebook_client::channel()
{
	handle_entry("channel");

	// Get update
	http::response resp = flap(REQUEST_MESSAGES_RECEIVE);

	if (resp.data.empty()) {
		// Something went wrong
		return handle_error("channel");
	}

	// Load traceId, if present
	std::string traceId = utils::text::source_get_value(&resp.data, 2, "\"tr\":\"", "\"");
	if (!traceId.empty()) {
		this->chat_traceid_ = traceId;
	}

	std::string type = utils::text::source_get_value(&resp.data, 2, "\"t\":\"", "\"");
	if (type == "continue" || type == "heartbeat") {
		// Everything is OK, no new message received
	}
	else if (type == "lb") {
		// Some new stuff (idk how does it work yet)
		this->chat_sticky_pool_ = utils::text::source_get_value(&resp.data, 2, "\"pool\":\"", "\"");
		parent->debugLogA("    Got self sticky pool: %s", this->chat_sticky_pool_.c_str());

		this->chat_sticky_num_ = utils::text::source_get_value2(&resp.data, "\"sticky\":\"", "\"");
		parent->debugLogA("    Got self sticky number: %s", this->chat_sticky_num_.c_str());
	}
	else if (type == "fullReload" || type == "refresh") {
		// Requested reload of page or relogin (due to some settings change, removing this session, etc.)
		parent->debugLogA("!!! Requested %s", type.c_str());

		this->chat_sequence_num_ = utils::text::source_get_value2(&resp.data, "\"seq\":", ",}");
		parent->debugLogA("    Got self sequence number: %s", this->chat_sequence_num_.c_str());

		if (type == "refresh") {
			this->chat_reconnect_reason_ = utils::text::source_get_value2(&resp.data, "\"reason\":", ",}");
			parent->debugLogA("    Got reconnect reason: %s", this->chat_reconnect_reason_.c_str());

			return this->reconnect();
		}
	}
	else if (!type.empty()) {
		// Something has been received, throw to new thread to process
		std::string* response_data = new std::string(resp.data);
		parent->ForkThread(&FacebookProto::ProcessMessages, response_data);

		// Get new sequence number
		std::string seq = utils::text::source_get_value2(&resp.data, "\"seq\":", ",}");
		parent->debugLogA("    Got self sequence number: %s", seq.c_str());

		if (type == "msg") {
			// Update msgs_recv number for every "msg" type we receive (during fullRefresh/reload responses it stays the same)
			this->chat_msgs_recv_++;
		}

		// Check if it's different from our old one (which means we should increment our old one)
		if (seq != this->chat_sequence_num_) {
			// Facebook now often return much bigger number which results in skipping few data requests, so we increment it manually
			// Bigger skips (when there is some problem or something) are handled when fullreload/refresh response type
			int iseq = 0;
			if (utils::conversion::from_string<int>(iseq, this->chat_sequence_num_, std::dec)) {
				// Increment and convert it back to string
				iseq++;
				std::string newSeq = utils::conversion::to_string(&iseq, UTILS_CONV_SIGNED_NUMBER);

				// Check if we have different seq than the one from Facebook
				if (newSeq != seq) {
					parent->debugLogA("!!! Use self incremented sequence number: %s (instead of: %s)", newSeq.c_str(), seq.c_str());
					seq = newSeq;
				}
			}
		}

		this->chat_sequence_num_ = seq;
	}
	else {
		// No type? This shouldn't happen unless there is a big API change.
		return handle_error("channel");
	}

	// Return
	switch (resp.code)
	{
	case HTTP_CODE_OK:
		return handle_success("channel");

	case HTTP_CODE_GATEWAY_TIMEOUT:
		// Maybe we have same clientid as other connected client, try to generate different one
		this->chat_clientid_ = utils::text::rand_string(8, "0123456789abcdef", &this->random_);

		// Intentionally fall to handle_error() below
	case HTTP_CODE_FAKE_DISCONNECTED:
	case HTTP_CODE_FAKE_ERROR:
	default:
		return handle_error("channel");
	}
}

bool facebook_client::activity_ping()
{
	// Don't send ping when we are not online
	if (parent->m_iStatus != ID_STATUS_ONLINE)
		return true;

	handle_entry("activity_ping");

	http::response resp = flap(REQUEST_ACTIVE_PING);

	// Remember this last ping time
	parent->m_pingTS = ::time(NULL);

	if (resp.data.empty() || resp.data.find("\"t\":\"pong\"") == resp.data.npos) {
		// Something went wrong
		return handle_error("activity_ping");
	}

	return handle_success("activity_ping");
}

int facebook_client::send_message(int seqid, MCONTACT hContact, const std::string &message_text, std::string *error_text, const std::string &captcha_persist_data, const std::string &captcha)
{
	handle_entry("send_message");

	http::response resp;
	std::string data;

	if (!captcha.empty()) {
		data += "&captcha_persist_data=" + captcha_persist_data;
		data += "&recaptcha_challenge_field=";
		data += "&captcha_response=" + captcha;
	}

	boolean isChatRoom = parent->isChatRoom(hContact);

	ptrA userId( parent->getStringA(hContact, FACEBOOK_KEY_ID));
	ptrA threadId( parent->getStringA(hContact, FACEBOOK_KEY_TID));
	
	// Check if we have userId/threadId to be able to send message
	if ((isChatRoom && (threadId == NULL || !mir_strcmp(threadId, "null")))
		|| (!isChatRoom && (userId == NULL || !mir_strcmp(userId, "null")))) {
		// This shouldn't happen unless user manually deletes some data via Database Editor++
		*error_text = Translate("Contact doesn't have required data in database.");

		handle_error("send_message");
		return SEND_MESSAGE_ERROR;
	}

	data += "&message_batch[0][action_type]=ma-type:user-generated-message";
	
	if (isChatRoom) {
		data += "&message_batch[0][thread_id]=" + std::string(threadId);
	} else {
		data += "&message_batch[0][specific_to_list][0]=fbid:" + std::string(userId);
		data += "&message_batch[0][specific_to_list][1]=fbid:" + this->self_.user_id;
		data += "&message_batch[0][other_user_fbid]=" + std::string(userId);
	}

	data += "&message_batch[0][author]=fbid:" + this->self_.user_id;
	data += "&message_batch[0][author_email]";
	data += "&message_batch[0][timestamp]=" + utils::time::mili_timestamp();
	data += "&message_batch[0][timestamp_absolute]";
	data += "&message_batch[0][timestamp_relative]";
	data += "&message_batch[0][timestamp_time_passed]";
	data += "&message_batch[0][is_unread]=false";
	data += "&message_batch[0][is_forward]=false";
	data += "&message_batch[0][is_filtered_content]=false";
	data += "&message_batch[0][is_filtered_content_bh]=false";
	data += "&message_batch[0][is_filtered_content_account]=false";
	data += "&message_batch[0][is_filtered_content_quasar]=false";
	data += "&message_batch[0][is_filtered_content_invalid_app]=false";
	data += "&message_batch[0][is_spoof_warning]=false";
	data += "&message_batch[0][source]=source:chat:web";
	data += "&message_batch[0][source_tags][0]=source:chat";

	// Experimental sticker sending support
	if (message_text.substr(0, 10) == "[[sticker:" && message_text.substr(message_text.length() - 2) == "]]") {
		data += "&message_batch[0][body]=";
		data += "&message_batch[0][sticker_id]=" + utils::url::encode(message_text.substr(10, message_text.length()-10-2));
	}
	else {
		data += "&message_batch[0][body]=" + utils::url::encode(message_text);
	}

	data += "&message_batch[0][has_attachment]=false";
	data += "&message_batch[0][html_body]=false";
	data += "&message_batch[0][signatureID]";
	data += "&message_batch[0][ui_push_phase]";
	data += "&message_batch[0][status]=0";
	data += "&message_batch[0][offline_threading_id]";
	data += "&message_batch[0][message_id]";
	data += "&message_batch[0][ephemeral_ttl_mode]=0";
	data += "&message_batch[0][manual_retry_cnt]=0";
	data += "&client=mercury&__a=1&__pc=EXP1:DEFAULT";
	data += "&fb_dtsg=" + this->dtsg_;
	data += "&__user=" + this->self_.user_id;
	data += "&ttstamp=" + ttstamp_;
	data += "&__dyn=" + __dyn();
	data += "&__req=" + __req();
	data += "&__rev=" + __rev();

	{
		ScopedLock s(send_message_lock_);
		resp = flap(REQUEST_MESSAGES_SEND, &data); // NOTE: Request revised 11.2.2016

		*error_text = resp.error_text;

		if (resp.error_number == 0) {
			// Everything is OK
			// Remember this message id
			std::string mid = utils::text::source_get_value(&resp.data, 2, "\"message_id\":\"", "\"");
			if (mid.empty())
				mid = utils::text::source_get_value(&resp.data, 2, "\"mid\":\"", "\""); // TODO: This is probably not used anymore

			// For classic contacts remember last message id
			if (!parent->isChatRoom(hContact))
				parent->setString(hContact, FACEBOOK_KEY_MESSAGE_ID, mid.c_str());

			// Get timestamp
			std::string timestamp = utils::text::source_get_value(&resp.data, 2, "\"timestamp\":", ",");
			time_t time = utils::time::from_string(timestamp);

			// Remember last action timestamp for history sync
			parent->setDword(FACEBOOK_KEY_LAST_ACTION_TS, time);

			// For classic conversation we try to replace timestamp of added event in OnPreCreateEvent()
			if (seqid > 0)
				messages_timestamp.insert(std::make_pair(seqid, time));

			// We have this message in database, so ignore further tries (in channel) to add it again
			messages_ignore.insert(std::make_pair(mid, 0));
		}
	}

	switch (resp.error_number) {
	case 0: 
		// Everything is OK
		break;

	// case 1356002: // You are offline (probably you can't use mercury or some other request when chat is offline)

	case 1356003: // Contact is offline
		parent->setWord(hContact, "Status", ID_STATUS_OFFLINE);
		return SEND_MESSAGE_ERROR;

	case 1356026: // Contact has alternative client
		client_notify(TranslateT("Need confirmation for sending messages to other clients.\nOpen Facebook website and try to send message to this contact again!"));
		return SEND_MESSAGE_ERROR;

	case 1357007: // Security check (captcha) is required
		{
			std::string imageUrl = utils::text::html_entities_decode(utils::text::slashu_to_utf8(utils::text::source_get_value(&resp.data, 3, "img class=\\\"img\\\"", "src=\\\"", "\\\"")));
			std::string captchaPersistData = utils::text::source_get_value(&resp.data, 3, "\\\"captcha_persist_data\\\"", "value=\\\"", "\\\"");

			parent->debugLogA("    Got imageUrl (first): %s", imageUrl.c_str());
			parent->debugLogA("    Got captchaPersistData (first): %s", captchaPersistData.c_str());

			std::string capStr = "new_captcha_type=TFBCaptcha&skipped_captcha_data=" + captchaPersistData;
			capStr += "&__dyn=" + __dyn();
			capStr += "&__req=" + __req();
			capStr += "&__rev=" + __rev();
			capStr += "&__user=" + this->self_.user_id;
			http::response capResp = flap(REQUEST_CAPTCHA_REFRESH, NULL, &capStr);

			if (capResp.code == HTTP_CODE_OK) {
				imageUrl = utils::text::html_entities_decode(utils::text::slashu_to_utf8(utils::text::source_get_value(&capResp.data, 3, "img class=\\\"img\\\"", "src=\\\"", "\\\"")));
				captchaPersistData = utils::text::source_get_value(&capResp.data, 3, "\\\"captcha_persist_data\\\"", "value=\\\"", "\\\"");

				parent->debugLogA("    Got imageUrl (second): %s", imageUrl.c_str());
				parent->debugLogA("    Got captchaPersistData (second): %s", captchaPersistData.c_str());

				std::string result;
				if (!parent->RunCaptchaForm(imageUrl, result)) {
					*error_text = Translate("User cancel captcha challenge.");
					return SEND_MESSAGE_CANCEL;
				}

				return send_message(seqid, hContact, message_text, error_text, captchaPersistData, result);
			}
		}
		return SEND_MESSAGE_CANCEL; // Cancel because we failed to load captcha image so we can't continue only with error

	//case 1404123: // Blocked sending messages (with URLs) because Facebook think our computer is infected with malware

	default: // Other error
		parent->debugLogA("!!! Send message error #%d: %s", resp.error_number, resp.error_text.c_str());
		return SEND_MESSAGE_ERROR;
	}

	switch (resp.code) {
	case HTTP_CODE_OK:
		handle_success("send_message");
		return SEND_MESSAGE_OK;

	case HTTP_CODE_FAKE_ERROR:
	case HTTP_CODE_FAKE_DISCONNECTED:
	default:
		*error_text = Translate("Timeout when sending message.");

		handle_error("send_message");
		return SEND_MESSAGE_ERROR;
	}
}

bool facebook_client::post_status(status_data *status)
{
	if (status == NULL || (status->text.empty() && status->url.empty()))
		return false;

	handle_entry("post_status");

	if (status->isPage) {
		std::string data = "fb_dtsg=" + this->dtsg_;
		data += "&user_id=" + status->user_id;
		data += "&url=" + std::string(FACEBOOK_URL_HOMEPAGE);
		flap(REQUEST_IDENTITY_SWITCH, &data);
	}

	std::string data;
	if (!status->url.empty()) {
		data = "fb_dtsg=" + this->dtsg_;
		data += "&targetid=" + (status->user_id.empty() ? this->self_.user_id : status->user_id);
		data += "&xhpc_targetid=" + (status->user_id.empty() ? this->self_.user_id : status->user_id);
		data += "&istimeline=1&composercontext=composer&onecolumn=1&nctr[_mod]=pagelet_timeline_recent&__a=1&ttstamp=" + ttstamp_;
		data += "&__user=" + (status->isPage && !status->user_id.empty() ? status->user_id : this->self_.user_id);
		data += "&loaded_components[0]=maininput&loaded_components[1]=backdateicon&loaded_components[2]=withtaggericon&loaded_components[3]=cameraicon&loaded_components[4]=placetaggericon&loaded_components[5]=mainprivacywidget&loaded_components[6]=withtaggericon&loaded_components[7]=backdateicon&loaded_components[8]=placetaggericon&loaded_components[9]=cameraicon&loaded_components[10]=mainprivacywidget&loaded_components[11]=maininput&loaded_components[12]=explicitplaceinput&loaded_components[13]=hiddenplaceinput&loaded_components[14]=placenameinput&loaded_components[15]=hiddensessionid&loaded_components[16]=withtagger&loaded_components[17]=backdatepicker&loaded_components[18]=placetagger&loaded_components[19]=citysharericon";
		http::response resp = flap(REQUEST_LINK_SCRAPER, &data, &status->url);
		std::string temp = utils::text::html_entities_decode(utils::text::slashu_to_utf8(resp.data));

		data = "&xhpc_context=profile&xhpc_ismeta=1&xhpc_timeline=1&xhpc_composerid=u_jsonp_2_0&is_explicit_place=&composertags_place=&composer_session_id=&composertags_city=&disable_location_sharing=false&composer_predicted_city=&nctr[_mod]=pagelet_composer&__a=1&__dyn=&__req=1f&ttstamp=" + ttstamp_;
		std::string form = utils::text::source_get_value(&temp, 2, "<form", "</form>");
		utils::text::replace_all(&form, "\\\"", "\"");
		data += "&" + utils::text::source_get_form_data(&form) + "&";
		//data += "&no_picture=0";
	}

	std::string text = utils::url::encode(status->text);

	data += "fb_dtsg=" + this->dtsg_;
	data += "&xhpc_targetid=" + (status->user_id.empty() ? this->self_.user_id : status->user_id);
	data += "&__user=" + (status->isPage && !status->user_id.empty() ? status->user_id : this->self_.user_id);
	data += "&xhpc_message=" + text;
	data += "&xhpc_message_text=" + text;
	if (!status->isPage)
		data += "&audience[0][value]=" + get_privacy_type();
	if (!status->place.empty()) {
		data += "&composertags_place_name=";
		data += utils::url::encode(status->place);
	}
	for (std::vector<facebook_user*>::size_type i = 0; i < status->users.size(); i++) {
		data += "&composertags_with[" + utils::conversion::to_string(&i, UTILS_CONV_UNSIGNED_NUMBER);
		data += "]=" + status->users[i]->user_id;
		data += "&text_composertags_with[" + utils::conversion::to_string(&i, UTILS_CONV_UNSIGNED_NUMBER);
		data += "]=" + status->users[i]->real_name;
		delete status->users[i];
	}
	status->users.clear();

	data += "&xhpc_context=profile&xhpc_ismeta=1&xhpc_timeline=1&xhpc_composerid=u_0_2y&is_explicit_place=&composertags_place=&composertags_city=";

	http::response resp = flap(REQUEST_POST_STATUS, &data);

	if (status->isPage) {
		std::string query = "fb_dtsg=" + this->dtsg_;
		query += "&user_id=" + this->self_.user_id;
		query += "&url=" + std::string(FACEBOOK_URL_HOMEPAGE);
		flap(REQUEST_IDENTITY_SWITCH, &query);
	}

	if (resp.isValid()) {
		parent->NotifyEvent(parent->m_tszUserName, TranslateT("Status update was successful."), NULL, FACEBOOK_EVENT_OTHER);
		return handle_success("post_status");
	}

	return handle_error("post_status");
}

//////////////////////////////////////////////////////////////////////////////

bool facebook_client::save_url(const std::string &url, const std::tstring &filename, HANDLE &nlc)
{
	NETLIBHTTPREQUEST req = { sizeof(req) };
	NETLIBHTTPREQUEST *resp;
	req.requestType = REQUEST_GET;
	req.szUrl = const_cast<char*>(url.c_str());
	req.flags = NLHRF_HTTP11 | NLHRF_REDIRECT | NLHRF_PERSISTENT | NLHRF_NODUMP;
	req.nlc = nlc;

	resp = reinterpret_cast<NETLIBHTTPREQUEST*>(CallService(MS_NETLIB_HTTPTRANSACTION,
		reinterpret_cast<WPARAM>(handle_), reinterpret_cast<LPARAM>(&req)));

	bool ret = false;

	if (resp) {
		nlc = resp->nlc;
		parent->debugLogA("@@@ Saving URL %s to file %s", url.c_str(), _T2A(filename.c_str()));

		// Create folder if necessary
		std::tstring dir = filename.substr(0, filename.rfind('\\'));
		if (_taccess(dir.c_str(), 0))
			CreateDirectoryTreeT(dir.c_str());

		// Write to file
		FILE *f = _tfopen(filename.c_str(), _T("wb"));
		if (f != NULL) {
			fwrite(resp->pData, 1, resp->dataLength, f);
			fclose(f);

			ret = _taccess(filename.c_str(), 0) == 0;
		}

		CallService(MS_NETLIB_FREEHTTPREQUESTSTRUCT, 0, (LPARAM)resp);
	}
	else {
		nlc = NULL;
	}

	return ret;
}

bool facebook_client::sms_code(const char *fb_dtsg)
{
	std::string inner_data = "method_requested=sms_requested";
	inner_data += "&current_time=" + (utils::time::unix_timestamp() + ".000");
	inner_data += "&__a=1";
	inner_data += "&__user=0";
	inner_data += "&__dyn=" + __dyn();
	inner_data += "&__req=" + __req();
	inner_data += "&__be=0";
	inner_data += "&__pc=EXP1:DEFAULT";
	inner_data += "&fb_dtsg=" + std::string(fb_dtsg);
	inner_data += "&ttstamp=" + ttstamp_;
	inner_data += "&__rev=" + __rev();
	http::response resp = flap(REQUEST_LOGIN_SMS, &inner_data);

	if (resp.data.find("\"is_valid\":true", 0) == std::string::npos) {
		// Code wasn't send
		client_notify(TranslateT("Error occurred when requesting verification SMS code."));
		return false;
	}

	parent->NotifyEvent(parent->m_tszUserName, TranslateT("Verification SMS code was sent to your mobile phone."), NULL, FACEBOOK_EVENT_OTHER);
	return true;
}