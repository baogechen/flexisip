/*
	Flexisip, a flexible SIP proxy server with media capabilities.
    Copyright (C) 2010  Belledonne Communications SARL.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as
    published by the Free Software Foundation, either version 3 of the
    License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "agent.hh"
#include "etchosts.hh"

class ForwardModule : public Module {
	public:
		ForwardModule(Agent *ag);
		virtual void onDeclare(ConfigStruct * module_config);
		virtual void onLoad(Agent *agent, const ConfigStruct *root);
		virtual void onRequest(SipEvent *ev);
		virtual void onResponse(SipEvent *ev);
		url_t* overrideDest(SipEvent *ev, url_t* dest);
		~ForwardModule();
	private:
		su_home_t mHome;
		sip_route_t *mOutRoute;
		bool mRewriteReqUri;
		static ModuleInfo<ForwardModule> sInfo;
};

ModuleInfo<ForwardModule> ForwardModule::sInfo("Forward",
   "This module executes the basic routing task of SIP requests and pass them to the transport layer. "
	"It must always be enabled.");


ForwardModule::ForwardModule(Agent *ag) : Module(ag){
	su_home_init(&mHome);
	mOutRoute=NULL;
}

ForwardModule::~ForwardModule(){
	su_home_deinit(&mHome);
}

void ForwardModule::onDeclare(ConfigStruct * module_config){
	ConfigItemDescriptor items[]={
			{	String	,	"route"	, 	"A sip uri where to send all requests",	""	},
			{	Boolean	,	"rewrite-req-uri"	,	"Rewrite request-uri's host and port according to above route", "false"	},
			config_item_end
	};
	module_config->addChildrenValues(items);
}

void ForwardModule::onLoad(Agent *agent, const ConfigStruct *module_config){
	std::string route=module_config->get<ConfigString>("route")->read();
	mRewriteReqUri=module_config->get<ConfigBoolean>("rewrite-req-uri")->read();
	if (route.size()>0){
		mOutRoute=sip_route_create(&mHome,(url_t*)route.c_str(),NULL);
		if (mOutRoute==NULL){
			LOGF("Bad route parameter '%s' in configuration of Forward module",route.c_str());
		}
	}
}

url_t* ForwardModule::overrideDest(SipEvent *ev, url_t *dest){
	if (mOutRoute){
		dest=mOutRoute->r_url;
		if (mRewriteReqUri){
			ev->mSip->sip_request->rq_url->url_host=mOutRoute->r_url->url_host;
			ev->mSip->sip_request->rq_url->url_port=mOutRoute->r_url->url_port;
		}
	}
	return dest;
}

void ForwardModule::onRequest(SipEvent *ev){
	size_t msg_size;
	char *buf;
	url_t* dest=NULL;
	sip_t *sip=ev->mSip;
	msg_t *msg=ev->mMsg;

	
	switch(sip->sip_request->rq_method){
		case sip_method_invite:
			LOGD("This is an invite");
			break;
		case sip_method_register:
			LOGD("This is a register");
			
		case sip_method_ack:
		default:
			break;
	}
	dest=sip->sip_request->rq_url;
	// removes top route header if it maches us
	if (sip->sip_route!=NULL){
		if (getAgent()->isUs(sip->sip_route->r_url)){
			sip_route_remove(msg,sip);
		}
		if (sip->sip_route!=NULL){
			/*forward to this route*/
			dest=sip->sip_route->r_url;
		}
	}

	/* workaround bad sip uris with two @ that results in host part being "something@somewhere" */
	if (strchr(dest->url_host,'@')!=0){
		nta_msg_treply (getSofiaAgent(),msg,400,"Bad request",SIPTAG_SERVER_STR(getAgent()->getServerString()),TAG_END());
		return;
	}
	
	dest=overrideDest(ev,dest);

	std::string ip;
	if (EtcHostsResolver::get()->resolve(dest->url_host,&ip)){
		LOGD("Found %s in /etc/hosts",dest->url_host);
		/* duplication dest because we don't want to modify the message with our name resolution result*/
		dest=url_hdup(ev->getHome(),dest);
		dest->url_host=ip.c_str();
	}

	if (!getAgent()->isUs(dest)) {
		buf=msg_as_string(ev->getHome(), msg, NULL, 0,&msg_size);
		LOGD("About to forward request to %s:\n%s",url_as_string(ev->getHome(),dest),buf);
		nta_msg_tsend (getSofiaAgent(),msg,(url_string_t*)dest,TAG_END());
	}else{
		LOGD("This message has final destination this proxy, discarded...");
		nta_msg_discard(getSofiaAgent(),msg);
	}
}


void ForwardModule::onResponse(SipEvent *ev){
	char *buf;
	size_t msg_size;

	buf=msg_as_string(ev->getHome(), ev->mMsg, NULL, 0,&msg_size);
	LOGD("About to forward response:\n%s",buf);
	
	nta_msg_tsend(getSofiaAgent(),ev->mMsg,(url_string_t*)NULL,TAG_END());
}
