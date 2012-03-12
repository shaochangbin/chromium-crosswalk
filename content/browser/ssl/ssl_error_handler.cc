// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/ssl/ssl_error_handler.h"

#include "base/bind.h"
#include "content/browser/renderer_host/render_view_host_impl.h"
#include "content/browser/renderer_host/resource_dispatcher_host_impl.h"
#include "content/browser/ssl/ssl_cert_error_handler.h"
#include "content/browser/tab_contents/navigation_controller_impl.h"
#include "content/browser/tab_contents/tab_contents.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/resource_request_info.h"
#include "net/base/net_errors.h"
#include "net/url_request/url_request.h"

using content::BrowserThread;
using content::RenderViewHostImpl;
using content::ResourceDispatcherHostImpl;
using content::ResourceRequestInfo;
using content::WebContents;

SSLErrorHandler::SSLErrorHandler(ResourceDispatcherHostImpl* host,
                                 net::URLRequest* request,
                                 ResourceType::Type resource_type)
    : manager_(NULL),
      request_id_(0, 0),
      resource_dispatcher_host_(host),
      request_url_(request->url()),
      resource_type_(resource_type),
      request_has_been_notified_(false) {
  DCHECK(!BrowserThread::CurrentlyOn(BrowserThread::UI));

  const ResourceRequestInfo* info = ResourceRequestInfo::ForRequest(request);
  request_id_.child_id = info->GetChildID();
  request_id_.request_id = info->GetRequestID();

  if (!info->GetAssociatedRenderView(&render_process_id_, &render_view_id_))
    NOTREACHED();

  // This makes sure we don't disappear on the IO thread until we've given an
  // answer to the net::URLRequest.
  //
  // Release in CompleteCancelRequest, CompleteContinueRequest, or
  // CompleteTakeNoAction.
  AddRef();
}

SSLErrorHandler::~SSLErrorHandler() {}

void SSLErrorHandler::OnDispatchFailed() {
  TakeNoAction();
}

void SSLErrorHandler::OnDispatched() {
  TakeNoAction();
}

SSLCertErrorHandler* SSLErrorHandler::AsSSLCertErrorHandler() {
  return NULL;
}

void SSLErrorHandler::Dispatch() {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));

  WebContents* web_contents = NULL;
  RenderViewHostImpl* render_view_host =
      RenderViewHostImpl::FromID(render_process_id_, render_view_id_);
  if (render_view_host)
    web_contents = render_view_host->GetDelegate()->GetAsWebContents();

  if (!web_contents) {
    // We arrived on the UI thread, but the tab we're looking for is no longer
    // here.
    OnDispatchFailed();
    return;
  }

  // Hand ourselves off to the SSLManager.
  manager_ =
      static_cast<NavigationControllerImpl*>(&web_contents->GetController())->
          ssl_manager();
  OnDispatched();
}

void SSLErrorHandler::CancelRequest() {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));

  // We need to complete this task on the IO thread.
  BrowserThread::PostTask(
      BrowserThread::IO, FROM_HERE,
      base::Bind(
          &SSLErrorHandler::CompleteCancelRequest, this, net::ERR_ABORTED));
}

void SSLErrorHandler::DenyRequest() {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));

  // We need to complete this task on the IO thread.
  BrowserThread::PostTask(
      BrowserThread::IO, FROM_HERE,
      base::Bind(
          &SSLErrorHandler::CompleteCancelRequest, this,
          net::ERR_INSECURE_RESPONSE));
}

void SSLErrorHandler::ContinueRequest() {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));

  // We need to complete this task on the IO thread.
  BrowserThread::PostTask(
      BrowserThread::IO, FROM_HERE,
      base::Bind(&SSLErrorHandler::CompleteContinueRequest, this));
}

void SSLErrorHandler::TakeNoAction() {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));

  // We need to complete this task on the IO thread.
  BrowserThread::PostTask(
      BrowserThread::IO, FROM_HERE,
      base::Bind(&SSLErrorHandler::CompleteTakeNoAction, this));
}

void SSLErrorHandler::CompleteCancelRequest(int error) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));

  // It is important that we notify the net::URLRequest only once.  If we try
  // to notify the request twice, it may no longer exist and |this| might have
  // already have been deleted.
  DCHECK(!request_has_been_notified_);
  if (request_has_been_notified_)
    return;

  net::URLRequest* request =
      resource_dispatcher_host_->GetURLRequest(request_id_);
  if (request && request->is_pending()) {
    // The request can be NULL if it was cancelled by the renderer (as the
    // result of the user navigating to a new page from the location bar).
    DVLOG(1) << "CompleteCancelRequest() url: " << request->url().spec();
    SSLCertErrorHandler* cert_error = AsSSLCertErrorHandler();
    if (cert_error)
      request->SimulateSSLError(error, cert_error->ssl_info());
    else
      request->SimulateError(error);
  }
  request_has_been_notified_ = true;

  // We're done with this object on the IO thread.
  Release();
}

void SSLErrorHandler::CompleteContinueRequest() {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));

  // It is important that we notify the net::URLRequest only once. If we try to
  // notify the request twice, it may no longer exist and |this| might have
  // already have been deleted.
  DCHECK(!request_has_been_notified_);
  if (request_has_been_notified_)
    return;

  net::URLRequest* request =
      resource_dispatcher_host_->GetURLRequest(request_id_);
  if (request) {
    // The request can be NULL if it was cancelled by the renderer (as the
    // result of the user navigating to a new page from the location bar).
    DVLOG(1) << "CompleteContinueRequest() url: " << request->url().spec();
    request->ContinueDespiteLastError();
  }
  request_has_been_notified_ = true;

  // We're done with this object on the IO thread.
  Release();
}

void SSLErrorHandler::CompleteTakeNoAction() {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));

  // It is important that we notify the net::URLRequest only once. If we try to
  // notify the request twice, it may no longer exist and |this| might have
  // already have been deleted.
  DCHECK(!request_has_been_notified_);
  if (request_has_been_notified_)
    return;

  request_has_been_notified_ = true;

  // We're done with this object on the IO thread.
  Release();
}
