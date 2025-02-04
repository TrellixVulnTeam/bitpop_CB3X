/*
 * Copyright (C) 2011 Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef FrameTestHelpers_h
#define FrameTestHelpers_h

#include "public/web/WebFrameClient.h"
#include "public/web/WebViewClient.h"
#include "web/WebViewImpl.h"
#include "wtf/PassOwnPtr.h"
#include <string>

namespace blink {

class WebLocalFrameImpl;
class WebSettings;

namespace FrameTestHelpers {

void loadFrame(WebFrame*, const std::string& url);
void runPendingTasks();

// Convenience class for handling the lifetime of a WebView and its associated mainframe in tests.
class WebViewHelper {
    WTF_MAKE_NONCOPYABLE(WebViewHelper);
public:
    WebViewHelper();
    ~WebViewHelper();

    // Creates and initializes the WebView. Implicitly calls reset() first. IF a
    // WebFrameClient or a WebViewClient are passed in, they must outlive the
    // WebViewHelper.
    WebViewImpl* initialize(bool enableJavascript = false, WebFrameClient* = 0, WebViewClient* = 0, void (*updateSettingsFunc)(WebSettings*) = 0);

    // Same as initialize() but also performs the initial load of the url.
    WebViewImpl* initializeAndLoad(const std::string& url, bool enableJavascript = false, WebFrameClient* = 0, WebViewClient* = 0, void (*updateSettingsFunc)(WebSettings*) = 0);

    void reset();

    WebView* webView() const { return m_webView; }
    WebViewImpl* webViewImpl() const { return m_webView; }

private:
    WebViewImpl* m_webView;
};

// Minimal implementation of WebFrameClient needed for unit tests that load frames. Tests that load
// frames and need further specialization of WebFrameClient behavior should subclass this.
class TestWebFrameClient : public WebFrameClient {
public:
    virtual WebFrame* createChildFrame(WebLocalFrame* parent, const WebString& frameName) OVERRIDE;
    virtual void frameDetached(WebFrame*) OVERRIDE;
};

class TestWebViewClient : public WebViewClient {
public:
    virtual ~TestWebViewClient() { }
    virtual void initializeLayerTreeView() OVERRIDE;
    virtual WebLayerTreeView* layerTreeView() OVERRIDE { return m_layerTreeView.get(); }

private:
    OwnPtr<WebLayerTreeView> m_layerTreeView;
};

} // namespace FrameTestHelpers
} // namespace blink

#endif // FrameTestHelpers_h
