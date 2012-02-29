/*
 Flexisip, a flexible SIP proxy server with media capabilities.
 Copyright (C) 2012  Belledonne Communications SARL.

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

#ifndef generic_contact_route_inserter_hh
#define generic_contact_route_inserter_hh

#include "agent.hh"

class GenericContactRouteInserter: public Module {
public:
	GenericContactRouteInserter(Agent *ag) :
			Module(ag) {

	}

	virtual void onDeclare(ConfigStruct *module_config) {
		ConfigItemDescriptor items[] = { { Boolean, "masquerade-contacts-for-invites", "Hack for workarounding Nortel CS2k gateways bug.", "false" }, config_item_end };
		module_config->addChildrenValues(items);
	}

	virtual void onLoad(Agent *agent, const ConfigStruct *module_config) {
		mContactRouteParamName = std::string("CtRt") + getAgent()->getUniqueId();
		mMasqueradeInviteContacts = module_config->get<ConfigBoolean>("masquerade-contacts-for-invites")->read();
	}

	virtual void onRequest(std::shared_ptr<SipEvent> &ev) {
		sip_t *sip = ev->mSip;

		if (sip->sip_request->rq_method == sip_method_register || ((sip->sip_request->rq_method == sip_method_invite) && mMasqueradeInviteContacts)) {
			masqueradeContact(ev);
		}
		if (sip->sip_request->rq_method != sip_method_register) {
			/* check if request-uri contains a contact-route parameter, so that we can route back to the client */
			char contact_route_param[64];
			url_t *dest = sip->sip_request->rq_url;
			// now need to check if request uri has special param inserted by contact-route-inserter module
			if (url_param(dest->url_params, mContactRouteParamName.c_str(), contact_route_param, sizeof(contact_route_param))) {
				//first remove param
				dest->url_params = url_strip_param_string(su_strdup(ev->getHome(), dest->url_params), mContactRouteParamName.c_str());
				//test and remove maddr param
				if (url_has_param(dest, "maddr")) {
					dest->url_params = url_strip_param_string(su_strdup(ev->getHome(), dest->url_params), "maddr");
				}
				//second change dest to
				char* tmp = strchr(contact_route_param, ':');
				if (tmp) {
					char* transport = su_strndup(ev->getHome(), contact_route_param, tmp - contact_route_param);
					char *tmp2 = tmp + 1;
					tmp = strchr(tmp2, ':');
					if (tmp) {
						dest->url_host = su_strndup(ev->getHome(), tmp2, tmp - tmp2);
						dest->url_port = su_strdup(ev->getHome(), tmp + 1);
						if (strcasecmp(transport, "udp") != 0) {
							char *t_param = su_sprintf(ev->getHome(), "transport=%s", transport);
							url_param_add(ev->getHome(), dest, t_param);
						}
					}
				}
			}
		}
	}
	virtual void onResponse(std::shared_ptr<SipEvent> &ev) {
		sip_t *sip = ev->mSip;
		if (mMasqueradeInviteContacts && (sip->sip_cseq->cs_method == sip_method_invite || sip->sip_cseq->cs_method == sip_method_subscribe)) {
			masqueradeContact(ev);
		}
	}
private:
        void masqueradeContact(std::shared_ptr<SipEvent> &ev){
                sip_t *sip=ev->mSip;
                if (sip->sip_contact!=NULL && sip->sip_contact->m_url!=NULL){
                        //rewrite contact, put local host instead and store previous contact host in new parameter
                        char ct_tport[32]="udp";
                        char* lParam;
                        url_t *ct_url=sip->sip_contact->m_url;

                        //grab the transport of the contact uri
                        if (url_param(sip->sip_contact->m_url->url_params,"transport",ct_tport,sizeof(ct_tport))>0){

                        }
                        /*add a parameter like "CtRt15.128.128.2=tcp:201.45.118.16:50025" in the contact, so that we know where is the client
                         when we later have to route an INVITE to him */
                        lParam=su_sprintf (ev->getHome(),"%s=%s:%s:%s",mContactRouteParamName.c_str()
                                                                                        ,ct_tport
                                                                                                        ,ct_url->url_host
                                                                                                        ,ct_url->url_port);
                        LOGD("Rewriting contact with param [%s]",lParam);
                        if (url_param_add (ev->getHome(),ct_url,lParam)) {
                                LOGE("Cannot insert url param [%s]",lParam);
                        }
                        /*masquerade the contact, so that later requests (INVITEs) come to us */
                        ct_url->url_host = getAgent()->getPublicIp().c_str();
                        ct_url->url_port = su_sprintf (ev->getHome(),"%i",getAgent()->getPort());
                        /*remove the transport, in most case further requests should come back to us in UDP*/
                        ct_url->url_params = url_strip_param_string(su_strdup(ev->getHome(),ct_url->url_params),"transport");
                }
        }

	std::string mContactRouteParamName;
	bool mMasqueradeInviteContacts;
};

#endif