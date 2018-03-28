/*

Jabber Protocol Plugin for Miranda NG

Copyright (c) 2018 Miranda NG team

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#include "stdafx.h"
#include "jabber_strm_mgmt.h"

strm_mgmt::strm_mgmt(CJabberProto *_proto) : proto(_proto), m_bStrmMgmtPendingEnable(false),
m_bStrmMgmtEnabled(false),
m_bStrmMgmtResumeSupported(false)
{

}

void strm_mgmt::OnProcessEnabled(HXML node, ThreadData * /*info*/)
{
	m_bStrmMgmtEnabled = true;
	auto val = XmlGetAttrValue(node, L"max");
	m_nStrmMgmtResumeMaxSeconds = _wtoi(val);
	val = XmlGetAttrValue(node, L"resume");
	if (mir_wstrcmp(val, L"true") || mir_wstrcmp(val, L"1"))
		m_bStrmMgmtResumeSupported = true;

	m_sStrmMgmtResumeId = XmlGetAttrValue(node, L"id");
	m_nStrmMgmtLocalHCount = 0;
	m_nStrmMgmtSrvHCount = 0;
}

void strm_mgmt::OnProcessSMa(HXML node)
{
	if (!mir_wstrcmp(XmlGetAttrValue(node, L"xmlns"), L"urn:xmpp:sm:3"))
	{
		auto val = XmlGetAttrValue(node, L"h");
		uint32_t iVal = _wtoi(val);
		m_nStrmMgmtSrvHCount = iVal;
		int size = m_nStrmMgmtLocalSCount - m_nStrmMgmtSrvHCount;
		if (size < 0)
		{
			//TODO: this should never happen, indicate server side bug
			//TODO: once our side implementation good enough abort strem in this case, noop for now
		}
		else if (size > 0)
		{
			const size_t diff = NodeCache.size() - size;
			if (diff)
			{
				size_t diff_tmp = diff;
				for (auto i : NodeCache)
				{
					if (diff_tmp > 0)
					{
						xmlFree(i);
						diff_tmp--;
					}
				}
				diff_tmp = diff;
				while (diff_tmp)
				{
					NodeCache.pop_front();
					diff_tmp--;
				}
			}
			for (auto i : NodeCache)
				proto->m_ThreadInfo->send(i);
		}
		NodeCache.clear();
	}
}

void strm_mgmt::OnProcessSMr(HXML node)
{
	if (!mir_wstrcmp(XmlGetAttrValue(node, L"xmlns"), L"urn:xmpp:sm:3"))
	{
		XmlNode enable_sm(L"a");
		XmlAddAttr(enable_sm, L"xmlns", L"urn:xmpp:sm:3");
		xmlAddAttrInt(enable_sm, L"h", m_nStrmMgmtLocalHCount);
		proto->m_ThreadInfo->send(enable_sm);
	}
}

void strm_mgmt::OnProcessFailed(HXML node, ThreadData * /*info*/) //used failed instead of failure, notes: https://xmpp.org/extensions/xep-0198.html#errors
{
	if (!mir_wstrcmp(XmlGetAttrValue(node, L"xmlns"), L"urn:xmpp:sm:3"))
	{
		//TODO: handle failure
	}
}

void strm_mgmt::CheckStreamFeatures(HXML node)
{
	if (!mir_wstrcmp(XmlGetName(node), L"sm"))
	{
		if (!mir_wstrcmp(XmlGetAttrValue(node, L"xmlns"), L"urn:xmpp:sm:3")) //we work only with version 3 or higher of sm
		{
			if (!(proto->m_bJabberOnline))
				m_bStrmMgmtPendingEnable = true;
			else
				EnableStrmMgmt();
		}
	}
}

void strm_mgmt::CheckState()
{
	if (m_bStrmMgmtPendingEnable)
		EnableStrmMgmt();
	//TODO: resume stream from here ?
}

void strm_mgmt::HandleOutgoingNode(HXML node)
{
	if (!m_bStrmMgmtEnabled)
		return;
	auto name = XmlGetName(node);
	if (mir_wstrcmp(name, L"a") && mir_wstrcmp(name, L"r"))
	{
		m_nStrmMgmtLocalSCount++;
		if ((m_nStrmMgmtLocalSCount - m_nStrmMgmtSrvHCount) >= m_nStrmMgmtCacheSize)
		{
			XmlNode enable_sm(L"r");
			XmlAddAttr(enable_sm, L"xmlns", L"urn:xmpp:sm:3");
			proto->m_ThreadInfo->send(enable_sm);
		}
	}
}

void strm_mgmt::OnDisconnect()
{
	//TODO: following should be redone once resumption implemented
	//reset state of stream management
	m_bStrmMgmtEnabled = false;
	m_bStrmMgmtPendingEnable = false;
	//reset stream management h counters
	m_nStrmMgmtLocalHCount = m_nStrmMgmtLocalSCount = m_nStrmMgmtSrvHCount = 0;
}

void strm_mgmt::HandleIncommingNode(HXML node)
{
	if (m_bStrmMgmtEnabled && mir_wstrcmp(XmlGetName(node), L"r") && mir_wstrcmp(XmlGetName(node), L"a")) //TODO: something better
	{
		NodeCache.push_back(xmlCopyNode(node));
		m_nStrmMgmtLocalHCount++;
	}
	else if (!mir_wstrcmp(XmlGetName(node), L"r"))
		OnProcessSMr(node);
	else if (!mir_wstrcmp(XmlGetName(node), L"a"))
		OnProcessSMa(node);
}

void strm_mgmt::EnableStrmMgmt()
{
	XmlNode enable_sm(L"enable");
	XmlAddAttr(enable_sm, L"xmlns", L"urn:xmpp:sm:3");
	XmlAddAttr(enable_sm, L"resume", L"true"); //enable resumption (most useful part of this xep)
	proto->m_ThreadInfo->send(enable_sm);
	m_nStrmMgmtLocalSCount = 1; //TODO: this MUST be 0, i have bug somewhere.
}