
#include "rutil/AndroidLogger.hxx"
#include "rutil/Log.hxx"
#include "rutil/Logger.hxx"
#include "rutil/Subsystem.hxx"
#include "resip/dum/ClientAuthManager.hxx"
#include "resip/dum/ClientRegistration.hxx"
#include "resip/dum/DialogUsageManager.hxx"
#include "resip/dum/InviteSessionHandler.hxx"
#include "resip/dum/MasterProfile.hxx"
#include "resip/dum/Profile.hxx"
#include "resip/dum/UserProfile.hxx"
#include "resip/dum/RegistrationHandler.hxx"
#include "resip/dum/ClientPagerMessage.hxx"
#include "resip/dum/ServerPagerMessage.hxx"

#include "resip/dum/DialogUsageManager.hxx"
#include "resip/dum/AppDialogSet.hxx"
#include "resip/dum/AppDialog.hxx"
#include "resip/dum/RegistrationHandler.hxx"
#include "resip/dum/PagerMessageHandler.hxx"
#include "resip/stack/PlainContents.hxx"

#include <iostream>
#include <string>
#include <sstream>

#include "org_resiprocate_android_basicmessage_SipStack.h"

using namespace std;
using namespace resip;

#define RESIPROCATE_SUBSYSTEM Subsystem::TEST

#define DEFAULT_REGISTRATION_EXPIRY 600

static SipStack *clientStack;
static DialogUsageManager *clientDum;

class RegListener : public ClientRegistrationHandler {
public:
        RegListener() : _registered(false) {};
        bool isRegistered() { return _registered; };

        virtual void onSuccess(ClientRegistrationHandle, const SipMessage& response)
    {
        cout << "client registered\n";
            _registered = true;
    }
        virtual void onRemoved(ClientRegistrationHandle, const SipMessage& response)
    {
        cout << "client regListener::onRemoved\n";
            exit(-1);
    }
        virtual void onFailure(ClientRegistrationHandle, const SipMessage& response)
    {
        cout << "client regListener::onFailure\n";
            exit(-1);
    }
    virtual int onRequestRetry(ClientRegistrationHandle, int retrySeconds, const SipMessage& response)
    {
        cout << "client regListener::onRequestRetry\n";
            exit(-1);
        return -1;
    }

protected:
        bool _registered;

};


class ClientMessageHandler : public ClientPagerMessageHandler {
public:
   ClientMessageHandler()
      : finished(false),
        successful(false)
   {
   };

   virtual void onSuccess(ClientPagerMessageHandle, const SipMessage& status)
   {
      InfoLog(<<"ClientMessageHandler::onSuccess\n");
      successful = true;
      finished = true;
   }

   virtual void onFailure(ClientPagerMessageHandle, const SipMessage& status, std::auto_ptr<Contents> contents)
   {
      ErrLog(<<"ClientMessageHandler::onFailure\n");
      successful = false;
      finished = true;
   }

   bool isFinished() { return finished; };
   bool isSuccessful() { return successful; };

private:
   bool finished;
   bool successful;
};

static JNIEnv *_env;
static jobject message_handler;
static jmethodID on_message_method;

class ServerMessageHandler : public ServerPagerMessageHandler
{
public:
        ServerMessageHandler() {};
        virtual void onMessageArrived(ServerPagerMessageHandle handle, const SipMessage& message)
    {

            SharedPtr<SipMessage> ok = handle->accept();
            handle->send(ok);

            Contents *body = message.getContents();
            InfoLog(<< "Got a message: " << *body);
            
            jstring _body = _env->NewStringUTF(body->getBodyData().c_str());
            jstring _sender = _env->NewStringUTF(message.header(h_From).uri().getAor().c_str());
            
            _env->CallObjectMethod(message_handler, on_message_method, _sender, _body);
            
    }
};


// A proper implementation must keep the SIP stack active as long as the
// app is running.

#ifdef __cplusplus
extern "C" {
#endif
JNIEXPORT void JNICALL Java_org_resiprocate_android_basicmessage_SipStack_sendMessage
  (JNIEnv *env, jobject jthis, jstring recipient, jstring body)
{
   const char *_recipient = env->GetStringUTFChars(recipient, 0);
   const char *_body = env->GetStringUTFChars(body, 0);
   
   try {

      InfoLog(<< "Sending MESSAGE\n");
      NameAddr naTo(_recipient);
      ClientPagerMessageHandle cpmh = clientDum->makePagerMessage(naTo);

      Data messageBody(_body);
      auto_ptr<Contents> content(new PlainContents(messageBody));
      cpmh.get()->page(content);

   }
   catch (exception& e)
   {
      cout << e.what() << endl;
   }
   catch(...)
   {
      cout << "some exception!" << endl;
   }
    
   env->ReleaseStringUTFChars(recipient, _recipient);
   env->ReleaseStringUTFChars(body, _body);
}

/*
 * Class:     org_resiprocate_android_basicmessage_SipStack
 * Method:    init
 * Signature: ()V
 */
JNIEXPORT void JNICALL Java_org_resiprocate_android_basicmessage_SipStack_init
  (JNIEnv *env, jobject jthis, jstring sipUser, jstring realm, jstring user, jstring password)
{
   const char *_sipUser = env->GetStringUTFChars(sipUser, 0);
   const char *_realm = env->GetStringUTFChars(realm, 0);
   const char *_user = env->GetStringUTFChars(user, 0);
   const char *_password = env->GetStringUTFChars(password, 0);

   AndroidLogger alog;
   Log::initialize(Log::Cout, Log::Stack, "SIP", alog);
   
   RegListener client;
   SharedPtr<MasterProfile> profile(new MasterProfile);
   auto_ptr<ClientAuthManager> clientAuth(new ClientAuthManager());

   clientStack = new SipStack();
   // stats service creates timers requiring extra wakeups, so we disable it
   clientStack->statisticsManagerEnabled() = false;
   clientDum = new DialogUsageManager(*clientStack);
   clientDum->addTransport(TCP, 5065);
   clientDum->setMasterProfile(profile);
   
   clientDum->setClientRegistrationHandler(&client);

   clientDum->setClientAuthManager(clientAuth);
   clientDum->getMasterProfile()->setDefaultRegistrationTime(DEFAULT_REGISTRATION_EXPIRY);
   clientDum->getMasterProfile()->addSupportedMethod(MESSAGE);
   clientDum->getMasterProfile()->addSupportedMimeType(MESSAGE, Mime("text", "plain"));
   
   ClientMessageHandler *cmh = new ClientMessageHandler();
   clientDum->setClientPagerMessageHandler(cmh);
   
   ServerMessageHandler *smh = new ServerMessageHandler();
   clientDum->setServerPagerMessageHandler(smh);
   
   NameAddr naFrom(_sipUser);
   profile->setDefaultFrom(naFrom);
   profile->setDigestCredential(_realm, _user, _password);
   
   SharedPtr<SipMessage> regMessage = clientDum->makeRegistration(naFrom);
   clientDum->send( regMessage );
   
   env->ReleaseStringUTFChars(sipUser, _sipUser);
   env->ReleaseStringUTFChars(realm, _realm);
   env->ReleaseStringUTFChars(user, _user);
   env->ReleaseStringUTFChars(password, _password);

}

/*
 * Class:     org_resiprocate_android_basicmessage_SipStack
 * Method:    handleEvents
 * Signature: ()V
 */
JNIEXPORT jlong JNICALL Java_org_resiprocate_android_basicmessage_SipStack_handleEvents
  (JNIEnv *env, jobject)
{
   // This is used by callbacks:
   _env = env;

   clientStack->process(10);
   while(clientDum->process());
   return clientStack->getTimeTillNextProcessMS();
}

/*
 * Class:     org_resiprocate_android_basicmessage_SipStack
 * Method:    done
 * Signature: ()V
 */
JNIEXPORT void JNICALL Java_org_resiprocate_android_basicmessage_SipStack_done
  (JNIEnv *, jobject)
{
   // FIXME: should destroy the stack here
}

/*
 * Class:     org_resiprocate_android_basicmessage_SipStack
 * Method:    setMessageHandler
 * Signature: (Lorg/resiprocate/android/basicmessage/MessageHandler;)V
 */
JNIEXPORT void JNICALL Java_org_resiprocate_android_basicmessage_SipStack_setMessageHandler
  (JNIEnv *env, jobject _this, jobject _message_handler)
{
   message_handler = env->NewGlobalRef(_message_handler);
   jclass objclass = env->GetObjectClass(_message_handler);
   on_message_method = env->GetMethodID(objclass, "onMessage", "(Ljava/lang/String;Ljava/lang/String;)V");
      if(on_message_method == 0){
          ErrLog( << "could not get method id!\n");
          return;
      }
      
   
}


#ifdef __cplusplus
}
#endif
