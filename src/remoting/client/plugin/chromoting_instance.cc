// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "remoting/client/plugin/chromoting_instance.h"

#include <algorithm>
#include <string>
#include <vector>

#include "base/bind.h"
#include "base/callback.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/lazy_instance.h"
#include "base/logging.h"
#include "base/strings/string_split.h"
#include "base/strings/stringprintf.h"
#include "base/synchronization/lock.h"
#include "base/threading/thread.h"
#include "base/values.h"
#include "crypto/random.h"
#include "jingle/glue/thread_wrapper.h"
#include "media/base/media.h"
#include "net/socket/ssl_server_socket.h"
#include "ppapi/cpp/completion_callback.h"
#include "ppapi/cpp/dev/url_util_dev.h"
#include "ppapi/cpp/image_data.h"
#include "ppapi/cpp/input_event.h"
#include "ppapi/cpp/rect.h"
#include "ppapi/cpp/var_array_buffer.h"
#include "ppapi/cpp/var_dictionary.h"
#include "remoting/base/constants.h"
#include "remoting/base/util.h"
#include "remoting/client/chromoting_client.h"
#include "remoting/client/client_config.h"
#include "remoting/client/frame_consumer_proxy.h"
#include "remoting/client/plugin/delegating_signal_strategy.h"
#include "remoting/client/plugin/media_source_video_renderer.h"
#include "remoting/client/plugin/pepper_audio_player.h"
#include "remoting/client/plugin/pepper_input_handler.h"
#include "remoting/client/plugin/pepper_port_allocator.h"
#include "remoting/client/plugin/pepper_token_fetcher.h"
#include "remoting/client/plugin/pepper_view.h"
#include "remoting/client/software_video_renderer.h"
#include "remoting/protocol/connection_to_host.h"
#include "remoting/protocol/host_stub.h"
#include "remoting/protocol/libjingle_transport_factory.h"
#include "third_party/libjingle/source/talk/base/helpers.h"
#include "third_party/libjingle/source/talk/base/ssladapter.h"
#include "url/gurl.h"

// Windows defines 'PostMessage', so we have to undef it.
#if defined(PostMessage)
#undef PostMessage
#endif

namespace remoting {

namespace {

// 32-bit BGRA is 4 bytes per pixel.
const int kBytesPerPixel = 4;

#if defined(ARCH_CPU_LITTLE_ENDIAN)
const uint32_t kPixelAlphaMask = 0xff000000;
#else  // !defined(ARCH_CPU_LITTLE_ENDIAN)
const uint32_t kPixelAlphaMask = 0x000000ff;
#endif  // !defined(ARCH_CPU_LITTLE_ENDIAN)

// Default DPI to assume for old clients that use notifyClientResolution.
const int kDefaultDPI = 96;

// Interval at which to sample performance statistics.
const int kPerfStatsIntervalMs = 1000;

// URL scheme used by Chrome apps and extensions.
const char kChromeExtensionUrlScheme[] = "chrome-extension";

// Maximum width and height of a mouse cursor supported by PPAPI.
const int kMaxCursorWidth = 32;
const int kMaxCursorHeight = 32;

#if defined(USE_OPENSSL)
// Size of the random seed blob used to initialize RNG in libjingle. Libjingle
// uses the seed only for OpenSSL builds. OpenSSL needs at least 32 bytes of
// entropy (see http://wiki.openssl.org/index.php/Random_Numbers), but stores
// 1039 bytes of state, so we initialize it with 1k or random data.
const int kRandomSeedSize = 1024;
#endif  // defined(USE_OPENSSL)

std::string ConnectionStateToString(protocol::ConnectionToHost::State state) {
  // Values returned by this function must match the
  // remoting.ClientSession.State enum in JS code.
  switch (state) {
    case protocol::ConnectionToHost::INITIALIZING:
      return "INITIALIZING";
    case protocol::ConnectionToHost::CONNECTING:
      return "CONNECTING";
    case protocol::ConnectionToHost::AUTHENTICATED:
      // Report the authenticated state as 'CONNECTING' to avoid changing
      // the interface between the plugin and webapp.
      return "CONNECTING";
    case protocol::ConnectionToHost::CONNECTED:
      return "CONNECTED";
    case protocol::ConnectionToHost::CLOSED:
      return "CLOSED";
    case protocol::ConnectionToHost::FAILED:
      return "FAILED";
  }
  NOTREACHED();
  return std::string();
}

// TODO(sergeyu): Ideally we should just pass ErrorCode to the webapp
// and let it handle it, but it would be hard to fix it now because
// client plugin and webapp versions may not be in sync. It should be
// easy to do after we are finished moving the client plugin to NaCl.
std::string ConnectionErrorToString(protocol::ErrorCode error) {
  // Values returned by this function must match the
  // remoting.ClientSession.Error enum in JS code.
  switch (error) {
    case protocol::OK:
      return "NONE";

    case protocol::PEER_IS_OFFLINE:
      return "HOST_IS_OFFLINE";

    case protocol::SESSION_REJECTED:
    case protocol::AUTHENTICATION_FAILED:
      return "SESSION_REJECTED";

    case protocol::INCOMPATIBLE_PROTOCOL:
      return "INCOMPATIBLE_PROTOCOL";

    case protocol::HOST_OVERLOAD:
      return "HOST_OVERLOAD";

    case protocol::CHANNEL_CONNECTION_ERROR:
    case protocol::SIGNALING_ERROR:
    case protocol::SIGNALING_TIMEOUT:
    case protocol::UNKNOWN_ERROR:
      return "NETWORK_FAILURE";
  }
  DLOG(FATAL) << "Unknown error code" << error;
  return std::string();
}

// Returns true if |pixel| is not completely transparent.
bool IsVisiblePixel(uint32_t pixel) {
  return (pixel & kPixelAlphaMask) != 0;
}

// Returns true if there is at least one visible pixel in the given range.
bool IsVisibleRow(const uint32_t* begin, const uint32_t* end) {
  return std::find_if(begin, end, &IsVisiblePixel) != end;
}

// This flag blocks LOGs to the UI if we're already in the middle of logging
// to the UI. This prevents a potential infinite loop if we encounter an error
// while sending the log message to the UI.
bool g_logging_to_plugin = false;
bool g_has_logging_instance = false;
base::LazyInstance<scoped_refptr<base::SingleThreadTaskRunner> >::Leaky
    g_logging_task_runner = LAZY_INSTANCE_INITIALIZER;
base::LazyInstance<base::WeakPtr<ChromotingInstance> >::Leaky
    g_logging_instance = LAZY_INSTANCE_INITIALIZER;
base::LazyInstance<base::Lock>::Leaky
    g_logging_lock = LAZY_INSTANCE_INITIALIZER;
logging::LogMessageHandlerFunction g_logging_old_handler = NULL;

}  // namespace

// String sent in the "hello" message to the webapp to describe features.
const char ChromotingInstance::kApiFeatures[] =
    "highQualityScaling injectKeyEvent sendClipboardItem remapKey trapKey "
    "notifyClientResolution pauseVideo pauseAudio asyncPin thirdPartyAuth "
    "pinlessAuth extensionMessage allowMouseLock mediaSourceRendering";

const char ChromotingInstance::kRequestedCapabilities[] = "";
const char ChromotingInstance::kSupportedCapabilities[] = "desktopShape";

bool ChromotingInstance::ParseAuthMethods(const std::string& auth_methods_str,
                                          ClientConfig* config) {
  std::vector<std::string> auth_methods;
  base::SplitString(auth_methods_str, ',', &auth_methods);
  for (std::vector<std::string>::iterator it = auth_methods.begin();
       it != auth_methods.end(); ++it) {
    protocol::AuthenticationMethod authentication_method =
        protocol::AuthenticationMethod::FromString(*it);
    if (authentication_method.is_valid())
      config->authentication_methods.push_back(authentication_method);
  }
  if (config->authentication_methods.empty()) {
    LOG(ERROR) << "No valid authentication methods specified.";
    return false;
  }

  return true;
}

ChromotingInstance::ChromotingInstance(PP_Instance pp_instance)
    : pp::Instance(pp_instance),
      initialized_(false),
      plugin_task_runner_(new PluginThreadTaskRunner(&plugin_thread_delegate_)),
      context_(plugin_task_runner_.get()),
      input_tracker_(&mouse_input_filter_),
      key_mapper_(&input_tracker_),
      normalizing_input_filter_(CreateNormalizingInputFilter(&key_mapper_)),
      input_handler_(this, normalizing_input_filter_.get()),
      use_async_pin_dialog_(false),
      use_media_source_rendering_(false),
      weak_factory_(this) {
  RequestInputEvents(PP_INPUTEVENT_CLASS_MOUSE | PP_INPUTEVENT_CLASS_WHEEL);
  RequestFilteringInputEvents(PP_INPUTEVENT_CLASS_KEYBOARD);

  // Resister this instance to handle debug log messsages.
  RegisterLoggingInstance();

#if defined(USE_OPENSSL)
  // Initialize random seed for libjingle. It's necessary only with OpenSSL.
  char random_seed[kRandomSeedSize];
  crypto::RandBytes(random_seed, sizeof(random_seed));
  talk_base::InitRandom(random_seed, sizeof(random_seed));
#else
  // Libjingle's SSL implementation is not really used, but it has to be
  // initialized for NSS builds to make sure that RNG is initialized in NSS,
  // because libjingle uses it.
  talk_base::InitializeSSL();
#endif  // !defined(USE_OPENSSL)

  // Send hello message.
  scoped_ptr<base::DictionaryValue> data(new base::DictionaryValue());
  data->SetInteger("apiVersion", kApiVersion);
  data->SetString("apiFeatures", kApiFeatures);
  data->SetInteger("apiMinVersion", kApiMinMessagingVersion);
  data->SetString("requestedCapabilities", kRequestedCapabilities);
  data->SetString("supportedCapabilities", kSupportedCapabilities);

  PostLegacyJsonMessage("hello", data.Pass());
}

ChromotingInstance::~ChromotingInstance() {
  DCHECK(plugin_task_runner_->BelongsToCurrentThread());

  // Unregister this instance so that debug log messages will no longer be sent
  // to it. This will stop all logging in all Chromoting instances.
  UnregisterLoggingInstance();

  // PepperView must be destroyed before the client.
  view_weak_factory_.reset();
  view_.reset();

  client_.reset();

  plugin_task_runner_->Quit();

  // Ensure that nothing touches the plugin thread delegate after this point.
  plugin_task_runner_->DetachAndRunShutdownLoop();

  // Stopping the context shuts down all chromoting threads.
  context_.Stop();
}

bool ChromotingInstance::Init(uint32_t argc,
                              const char* argn[],
                              const char* argv[]) {
  CHECK(!initialized_);
  initialized_ = true;

  VLOG(1) << "Started ChromotingInstance::Init";

  // Check to make sure the media library is initialized.
  // http://crbug.com/91521.
  if (!media::IsMediaLibraryInitialized()) {
    LOG(ERROR) << "Media library not initialized.";
    return false;
  }

  // Check that the calling content is part of an app or extension.
  if (!IsCallerAppOrExtension()) {
    LOG(ERROR) << "Not an app or extension";
    return false;
  }

  // Start all the threads.
  context_.Start();

  return true;
}

void ChromotingInstance::HandleMessage(const pp::Var& message) {
  if (!message.is_string()) {
    LOG(ERROR) << "Received a message that is not a string.";
    return;
  }

  scoped_ptr<base::Value> json(
      base::JSONReader::Read(message.AsString(),
                             base::JSON_ALLOW_TRAILING_COMMAS));
  base::DictionaryValue* message_dict = NULL;
  std::string method;
  base::DictionaryValue* data = NULL;
  if (!json.get() ||
      !json->GetAsDictionary(&message_dict) ||
      !message_dict->GetString("method", &method) ||
      !message_dict->GetDictionary("data", &data)) {
    LOG(ERROR) << "Received invalid message:" << message.AsString();
    return;
  }

  if (method == "connect") {
    HandleConnect(*data);
  } else if (method == "disconnect") {
    HandleDisconnect(*data);
  } else if (method == "incomingIq") {
    HandleOnIncomingIq(*data);
  } else if (method == "releaseAllKeys") {
    HandleReleaseAllKeys(*data);
  } else if (method == "injectKeyEvent") {
    HandleInjectKeyEvent(*data);
  } else if (method == "remapKey") {
    HandleRemapKey(*data);
  } else if (method == "trapKey") {
    HandleTrapKey(*data);
  } else if (method == "sendClipboardItem") {
    HandleSendClipboardItem(*data);
  } else if (method == "notifyClientResolution") {
    HandleNotifyClientResolution(*data);
  } else if (method == "pauseVideo") {
    HandlePauseVideo(*data);
  } else if (method == "pauseAudio") {
    HandlePauseAudio(*data);
  } else if (method == "useAsyncPinDialog") {
    use_async_pin_dialog_ = true;
  } else if (method == "onPinFetched") {
    HandleOnPinFetched(*data);
  } else if (method == "onThirdPartyTokenFetched") {
    HandleOnThirdPartyTokenFetched(*data);
  } else if (method == "requestPairing") {
    HandleRequestPairing(*data);
  } else if (method == "extensionMessage") {
    HandleExtensionMessage(*data);
  } else if (method == "allowMouseLock") {
    HandleAllowMouseLockMessage();
  } else if (method == "enableMediaSourceRendering") {
    HandleEnableMediaSourceRendering();
  }
}

void ChromotingInstance::DidChangeFocus(bool has_focus) {
  DCHECK(plugin_task_runner_->BelongsToCurrentThread());

  input_handler_.DidChangeFocus(has_focus);
}

void ChromotingInstance::DidChangeView(const pp::View& view) {
  DCHECK(plugin_task_runner_->BelongsToCurrentThread());

  plugin_view_ = view;
  mouse_input_filter_.set_input_size(
      webrtc::DesktopSize(view.GetRect().width(), view.GetRect().height()));

  if (view_)
    view_->SetView(view);
}

bool ChromotingInstance::HandleInputEvent(const pp::InputEvent& event) {
  DCHECK(plugin_task_runner_->BelongsToCurrentThread());

  if (!IsConnected())
    return false;

  return input_handler_.HandleInputEvent(event);
}

void ChromotingInstance::SetDesktopSize(const webrtc::DesktopSize& size,
                                        const webrtc::DesktopVector& dpi) {
  mouse_input_filter_.set_output_size(size);

  scoped_ptr<base::DictionaryValue> data(new base::DictionaryValue());
  data->SetInteger("width", size.width());
  data->SetInteger("height", size.height());
  if (dpi.x())
    data->SetInteger("x_dpi", dpi.x());
  if (dpi.y())
    data->SetInteger("y_dpi", dpi.y());
  PostLegacyJsonMessage("onDesktopSize", data.Pass());
}

void ChromotingInstance::SetDesktopShape(const webrtc::DesktopRegion& shape) {
  if (desktop_shape_ && shape.Equals(*desktop_shape_))
    return;

  desktop_shape_.reset(new webrtc::DesktopRegion(shape));

  scoped_ptr<base::ListValue> rects_value(new base::ListValue());
  for (webrtc::DesktopRegion::Iterator i(shape); !i.IsAtEnd(); i.Advance()) {
    const webrtc::DesktopRect& rect = i.rect();
    scoped_ptr<base::ListValue> rect_value(new base::ListValue());
    rect_value->AppendInteger(rect.left());
    rect_value->AppendInteger(rect.top());
    rect_value->AppendInteger(rect.width());
    rect_value->AppendInteger(rect.height());
    rects_value->Append(rect_value.release());
  }

  scoped_ptr<base::DictionaryValue> data(new base::DictionaryValue());
  data->Set("rects", rects_value.release());
  PostLegacyJsonMessage("onDesktopShape", data.Pass());
}

void ChromotingInstance::OnConnectionState(
    protocol::ConnectionToHost::State state,
    protocol::ErrorCode error) {
  scoped_ptr<base::DictionaryValue> data(new base::DictionaryValue());
  data->SetString("state", ConnectionStateToString(state));
  data->SetString("error", ConnectionErrorToString(error));
  PostLegacyJsonMessage("onConnectionStatus", data.Pass());
}

void ChromotingInstance::FetchThirdPartyToken(
    const GURL& token_url,
    const std::string& host_public_key,
    const std::string& scope,
    base::WeakPtr<PepperTokenFetcher> pepper_token_fetcher) {
  // Once the Session object calls this function, it won't continue the
  // authentication until the callback is called (or connection is canceled).
  // So, it's impossible to reach this with a callback already registered.
  DCHECK(!pepper_token_fetcher_.get());
  pepper_token_fetcher_ = pepper_token_fetcher;
  scoped_ptr<base::DictionaryValue> data(new base::DictionaryValue());
  data->SetString("tokenUrl", token_url.spec());
  data->SetString("hostPublicKey", host_public_key);
  data->SetString("scope", scope);
  PostLegacyJsonMessage("fetchThirdPartyToken", data.Pass());
}

void ChromotingInstance::OnConnectionReady(bool ready) {
  scoped_ptr<base::DictionaryValue> data(new base::DictionaryValue());
  data->SetBoolean("ready", ready);
  PostLegacyJsonMessage("onConnectionReady", data.Pass());
}

void ChromotingInstance::OnRouteChanged(const std::string& channel_name,
                                        const protocol::TransportRoute& route) {
  scoped_ptr<base::DictionaryValue> data(new base::DictionaryValue());
  std::string message = "Channel " + channel_name + " using " +
      protocol::TransportRoute::GetTypeString(route.type) + " connection.";
  data->SetString("message", message);
  PostLegacyJsonMessage("logDebugMessage", data.Pass());
}

void ChromotingInstance::SetCapabilities(const std::string& capabilities) {
  scoped_ptr<base::DictionaryValue> data(new base::DictionaryValue());
  data->SetString("capabilities", capabilities);
  PostLegacyJsonMessage("setCapabilities", data.Pass());
}

void ChromotingInstance::SetPairingResponse(
    const protocol::PairingResponse& pairing_response) {
  scoped_ptr<base::DictionaryValue> data(new base::DictionaryValue());
  data->SetString("clientId", pairing_response.client_id());
  data->SetString("sharedSecret", pairing_response.shared_secret());
  PostLegacyJsonMessage("pairingResponse", data.Pass());
}

void ChromotingInstance::DeliverHostMessage(
    const protocol::ExtensionMessage& message) {
  scoped_ptr<base::DictionaryValue> data(new base::DictionaryValue());
  data->SetString("type", message.type());
  data->SetString("data", message.data());
  PostLegacyJsonMessage("extensionMessage", data.Pass());
}

void ChromotingInstance::FetchSecretFromDialog(
    bool pairing_supported,
    const protocol::SecretFetchedCallback& secret_fetched_callback) {
  // Once the Session object calls this function, it won't continue the
  // authentication until the callback is called (or connection is canceled).
  // So, it's impossible to reach this with a callback already registered.
  DCHECK(secret_fetched_callback_.is_null());
  secret_fetched_callback_ = secret_fetched_callback;
  scoped_ptr<base::DictionaryValue> data(new base::DictionaryValue());
  data->SetBoolean("pairingSupported", pairing_supported);
  PostLegacyJsonMessage("fetchPin", data.Pass());
}

void ChromotingInstance::FetchSecretFromString(
    const std::string& shared_secret,
    bool pairing_supported,
    const protocol::SecretFetchedCallback& secret_fetched_callback) {
  secret_fetched_callback.Run(shared_secret);
}

protocol::ClipboardStub* ChromotingInstance::GetClipboardStub() {
  // TODO(sergeyu): Move clipboard handling to a separate class.
  // crbug.com/138108
  return this;
}

protocol::CursorShapeStub* ChromotingInstance::GetCursorShapeStub() {
  // TODO(sergeyu): Move cursor shape code to a separate class.
  // crbug.com/138108
  return this;
}

scoped_ptr<protocol::ThirdPartyClientAuthenticator::TokenFetcher>
ChromotingInstance::GetTokenFetcher(const std::string& host_public_key) {
  return scoped_ptr<protocol::ThirdPartyClientAuthenticator::TokenFetcher>(
      new PepperTokenFetcher(weak_factory_.GetWeakPtr(), host_public_key));
}

void ChromotingInstance::InjectClipboardEvent(
    const protocol::ClipboardEvent& event) {
  scoped_ptr<base::DictionaryValue> data(new base::DictionaryValue());
  data->SetString("mimeType", event.mime_type());
  data->SetString("item", event.data());
  PostLegacyJsonMessage("injectClipboardItem", data.Pass());
}

void ChromotingInstance::SetCursorShape(
    const protocol::CursorShapeInfo& cursor_shape) {
  COMPILE_ASSERT(sizeof(uint32_t) == kBytesPerPixel, rgba_pixels_are_32bit);

  // pp::MouseCursor requires image to be in the native format.
  if (pp::ImageData::GetNativeImageDataFormat() !=
      PP_IMAGEDATAFORMAT_BGRA_PREMUL) {
    LOG(WARNING) << "Unable to set cursor shape - native image format is not"
                    " premultiplied BGRA";
    return;
  }

  int width = cursor_shape.width();
  int height = cursor_shape.height();

  int hotspot_x = cursor_shape.hotspot_x();
  int hotspot_y = cursor_shape.hotspot_y();
  int bytes_per_row = width * kBytesPerPixel;
  int src_stride = width;
  const uint32_t* src_row_data = reinterpret_cast<const uint32_t*>(
      cursor_shape.data().data());
  const uint32_t* src_row_data_end = src_row_data + src_stride * height;

  scoped_ptr<pp::ImageData> cursor_image;
  pp::Point cursor_hotspot;

  // Check if the cursor is visible.
  if (IsVisibleRow(src_row_data, src_row_data_end)) {
    // If the cursor exceeds the size permitted by PPAPI then crop it, keeping
    // the hotspot as close to the center of the new cursor shape as possible.
    if (height > kMaxCursorHeight) {
      int y = hotspot_y - (kMaxCursorHeight / 2);
      y = std::max(y, 0);
      y = std::min(y, height - kMaxCursorHeight);

      src_row_data += src_stride * y;
      height = kMaxCursorHeight;
      hotspot_y -= y;
    }
    if (width > kMaxCursorWidth) {
      int x = hotspot_x - (kMaxCursorWidth / 2);
      x = std::max(x, 0);
      x = std::min(x, height - kMaxCursorWidth);

      src_row_data += x;
      width = kMaxCursorWidth;
      bytes_per_row = width * kBytesPerPixel;
      hotspot_x -= x;
    }

    cursor_image.reset(new pp::ImageData(this, PP_IMAGEDATAFORMAT_BGRA_PREMUL,
                                          pp::Size(width, height), false));
    cursor_hotspot = pp::Point(hotspot_x, hotspot_y);

    uint8* dst_row_data = reinterpret_cast<uint8*>(cursor_image->data());
    for (int row = 0; row < height; row++) {
      memcpy(dst_row_data, src_row_data, bytes_per_row);
      src_row_data += src_stride;
      dst_row_data += cursor_image->stride();
    }
  }

  input_handler_.SetMouseCursor(cursor_image.Pass(), cursor_hotspot);
}

void ChromotingInstance::OnFirstFrameReceived() {
  scoped_ptr<base::DictionaryValue> data(new base::DictionaryValue());
  PostLegacyJsonMessage("onFirstFrameReceived", data.Pass());
}

void ChromotingInstance::HandleConnect(const base::DictionaryValue& data) {
  ClientConfig config;
  std::string local_jid;
  std::string auth_methods;
  if (!data.GetString("hostJid", &config.host_jid) ||
      !data.GetString("hostPublicKey", &config.host_public_key) ||
      !data.GetString("localJid", &local_jid) ||
      !data.GetString("authenticationMethods", &auth_methods) ||
      !ParseAuthMethods(auth_methods, &config) ||
      !data.GetString("authenticationTag", &config.authentication_tag)) {
    LOG(ERROR) << "Invalid connect() data.";
    return;
  }
  data.GetString("clientPairingId", &config.client_pairing_id);
  data.GetString("clientPairedSecret", &config.client_paired_secret);
  if (use_async_pin_dialog_) {
    config.fetch_secret_callback =
        base::Bind(&ChromotingInstance::FetchSecretFromDialog,
                   weak_factory_.GetWeakPtr());
  } else {
    std::string shared_secret;
    if (!data.GetString("sharedSecret", &shared_secret)) {
      LOG(ERROR) << "sharedSecret not specified in connect().";
      return;
    }
    config.fetch_secret_callback =
        base::Bind(&ChromotingInstance::FetchSecretFromString, shared_secret);
  }

  // Read the list of capabilities, if any.
  if (data.HasKey("capabilities")) {
    if (!data.GetString("capabilities", &config.capabilities)) {
      LOG(ERROR) << "Invalid connect() data.";
      return;
    }
  }

  ConnectWithConfig(config, local_jid);
}

void ChromotingInstance::ConnectWithConfig(const ClientConfig& config,
                                           const std::string& local_jid) {
  DCHECK(plugin_task_runner_->BelongsToCurrentThread());

  jingle_glue::JingleThreadWrapper::EnsureForCurrentMessageLoop();

  if (use_media_source_rendering_) {
    video_renderer_.reset(new MediaSourceVideoRenderer(this));
  } else {
    view_.reset(new PepperView(this, &context_));
    view_weak_factory_.reset(
        new base::WeakPtrFactory<FrameConsumer>(view_.get()));

    // SoftwareVideoRenderer runs on a separate thread so for now we wrap
    // PepperView with a ref-counted proxy object.
    scoped_refptr<FrameConsumerProxy> consumer_proxy =
        new FrameConsumerProxy(plugin_task_runner_,
                               view_weak_factory_->GetWeakPtr());

    SoftwareVideoRenderer* renderer =
        new SoftwareVideoRenderer(context_.main_task_runner(),
                                  context_.decode_task_runner(),
                                  consumer_proxy);
    view_->Initialize(renderer);
    if (!plugin_view_.is_null())
      view_->SetView(plugin_view_);
    video_renderer_.reset(renderer);
  }

  host_connection_.reset(new protocol::ConnectionToHost(true));
  scoped_ptr<AudioPlayer> audio_player(new PepperAudioPlayer(this));
  client_.reset(new ChromotingClient(config, &context_, host_connection_.get(),
                                     this, video_renderer_.get(),
                                     audio_player.Pass()));

  // Connect the input pipeline to the protocol stub & initialize components.
  mouse_input_filter_.set_input_stub(host_connection_->input_stub());
  if (!plugin_view_.is_null()) {
    mouse_input_filter_.set_input_size(webrtc::DesktopSize(
        plugin_view_.GetRect().width(), plugin_view_.GetRect().height()));
  }

  VLOG(0) << "Connecting to " << config.host_jid
          << ". Local jid: " << local_jid << ".";

  // Setup the signal strategy.
  signal_strategy_.reset(new DelegatingSignalStrategy(
      local_jid, base::Bind(&ChromotingInstance::SendOutgoingIq,
                            weak_factory_.GetWeakPtr())));

  scoped_ptr<cricket::HttpPortAllocatorBase> port_allocator(
      PepperPortAllocator::Create(this));
  scoped_ptr<protocol::TransportFactory> transport_factory(
      new protocol::LibjingleTransportFactory(
          signal_strategy_.get(), port_allocator.Pass(),
          NetworkSettings(NetworkSettings::NAT_TRAVERSAL_FULL)));

  // Kick off the connection.
  client_->Start(signal_strategy_.get(), transport_factory.Pass());

  // Start timer that periodically sends perf stats.
  plugin_task_runner_->PostDelayedTask(
      FROM_HERE, base::Bind(&ChromotingInstance::SendPerfStats,
                            weak_factory_.GetWeakPtr()),
      base::TimeDelta::FromMilliseconds(kPerfStatsIntervalMs));
}

void ChromotingInstance::HandleDisconnect(const base::DictionaryValue& data) {
  DCHECK(plugin_task_runner_->BelongsToCurrentThread());

  // PepperView must be destroyed before the client.
  view_weak_factory_.reset();
  view_.reset();

  VLOG(0) << "Disconnecting from host.";

  client_.reset();

  // Disconnect the input pipeline and teardown the connection.
  mouse_input_filter_.set_input_stub(NULL);
  host_connection_.reset();
}

void ChromotingInstance::HandleOnIncomingIq(const base::DictionaryValue& data) {
  std::string iq;
  if (!data.GetString("iq", &iq)) {
    LOG(ERROR) << "Invalid incomingIq() data.";
    return;
  }

  // Just ignore the message if it's received before Connect() is called. It's
  // likely to be a leftover from a previous session, so it's safe to ignore it.
  if (signal_strategy_)
    signal_strategy_->OnIncomingMessage(iq);
}

void ChromotingInstance::HandleReleaseAllKeys(
    const base::DictionaryValue& data) {
  if (IsConnected())
    input_tracker_.ReleaseAll();
}

void ChromotingInstance::HandleInjectKeyEvent(
      const base::DictionaryValue& data) {
  int usb_keycode = 0;
  bool is_pressed = false;
  if (!data.GetInteger("usbKeycode", &usb_keycode) ||
      !data.GetBoolean("pressed", &is_pressed)) {
    LOG(ERROR) << "Invalid injectKeyEvent.";
    return;
  }

  protocol::KeyEvent event;
  event.set_usb_keycode(usb_keycode);
  event.set_pressed(is_pressed);

  // Inject after the KeyEventMapper, so the event won't get mapped or trapped.
  if (IsConnected())
    input_tracker_.InjectKeyEvent(event);
}

void ChromotingInstance::HandleRemapKey(const base::DictionaryValue& data) {
  int from_keycode = 0;
  int to_keycode = 0;
  if (!data.GetInteger("fromKeycode", &from_keycode) ||
      !data.GetInteger("toKeycode", &to_keycode)) {
    LOG(ERROR) << "Invalid remapKey.";
    return;
  }

  key_mapper_.RemapKey(from_keycode, to_keycode);
}

void ChromotingInstance::HandleTrapKey(const base::DictionaryValue& data) {
  int keycode = 0;
  bool trap = false;
  if (!data.GetInteger("keycode", &keycode) ||
      !data.GetBoolean("trap", &trap)) {
    LOG(ERROR) << "Invalid trapKey.";
    return;
  }

  key_mapper_.TrapKey(keycode, trap);
}

void ChromotingInstance::HandleSendClipboardItem(
    const base::DictionaryValue& data) {
  std::string mime_type;
  std::string item;
  if (!data.GetString("mimeType", &mime_type) ||
      !data.GetString("item", &item)) {
    LOG(ERROR) << "Invalid sendClipboardItem data.";
    return;
  }
  if (!IsConnected()) {
    return;
  }
  protocol::ClipboardEvent event;
  event.set_mime_type(mime_type);
  event.set_data(item);
  host_connection_->clipboard_stub()->InjectClipboardEvent(event);
}

void ChromotingInstance::HandleNotifyClientResolution(
    const base::DictionaryValue& data) {
  int width = 0;
  int height = 0;
  int x_dpi = kDefaultDPI;
  int y_dpi = kDefaultDPI;
  if (!data.GetInteger("width", &width) ||
      !data.GetInteger("height", &height) ||
      !data.GetInteger("x_dpi", &x_dpi) ||
      !data.GetInteger("y_dpi", &y_dpi) ||
      width <= 0 || height <= 0 ||
      x_dpi <= 0 || y_dpi <= 0) {
    LOG(ERROR) << "Invalid notifyClientResolution.";
    return;
  }

  if (!IsConnected()) {
    return;
  }

  protocol::ClientResolution client_resolution;
  client_resolution.set_width(width);
  client_resolution.set_height(height);
  client_resolution.set_x_dpi(x_dpi);
  client_resolution.set_y_dpi(y_dpi);

  // Include the legacy width & height in DIPs for use by older hosts.
  client_resolution.set_dips_width((width * kDefaultDPI) / x_dpi);
  client_resolution.set_dips_height((height * kDefaultDPI) / y_dpi);

  host_connection_->host_stub()->NotifyClientResolution(client_resolution);
}

void ChromotingInstance::HandlePauseVideo(const base::DictionaryValue& data) {
  bool pause = false;
  if (!data.GetBoolean("pause", &pause)) {
    LOG(ERROR) << "Invalid pauseVideo.";
    return;
  }
  if (!IsConnected()) {
    return;
  }
  protocol::VideoControl video_control;
  video_control.set_enable(!pause);
  host_connection_->host_stub()->ControlVideo(video_control);
}

void ChromotingInstance::HandlePauseAudio(const base::DictionaryValue& data) {
  bool pause = false;
  if (!data.GetBoolean("pause", &pause)) {
    LOG(ERROR) << "Invalid pauseAudio.";
    return;
  }
  if (!IsConnected()) {
    return;
  }
  protocol::AudioControl audio_control;
  audio_control.set_enable(!pause);
  host_connection_->host_stub()->ControlAudio(audio_control);
}
void ChromotingInstance::HandleOnPinFetched(const base::DictionaryValue& data) {
  std::string pin;
  if (!data.GetString("pin", &pin)) {
    LOG(ERROR) << "Invalid onPinFetched.";
    return;
  }
  if (!secret_fetched_callback_.is_null()) {
    secret_fetched_callback_.Run(pin);
    secret_fetched_callback_.Reset();
  } else {
    LOG(WARNING) << "Ignored OnPinFetched received without a pending fetch.";
  }
}

void ChromotingInstance::HandleOnThirdPartyTokenFetched(
    const base::DictionaryValue& data) {
  std::string token;
  std::string shared_secret;
  if (!data.GetString("token", &token) ||
      !data.GetString("sharedSecret", &shared_secret)) {
    LOG(ERROR) << "Invalid onThirdPartyTokenFetched data.";
    return;
  }
  if (pepper_token_fetcher_.get()) {
    pepper_token_fetcher_->OnTokenFetched(token, shared_secret);
    pepper_token_fetcher_.reset();
  } else {
    LOG(WARNING) << "Ignored OnThirdPartyTokenFetched without a pending fetch.";
  }
}

void ChromotingInstance::HandleRequestPairing(
    const base::DictionaryValue& data) {
  std::string client_name;
  if (!data.GetString("clientName", &client_name)) {
    LOG(ERROR) << "Invalid requestPairing";
    return;
  }
  if (!IsConnected()) {
    return;
  }
  protocol::PairingRequest pairing_request;
  pairing_request.set_client_name(client_name);
  host_connection_->host_stub()->RequestPairing(pairing_request);
}

void ChromotingInstance::HandleExtensionMessage(
    const base::DictionaryValue& data) {
  std::string type;
  std::string message_data;
  if (!data.GetString("type", &type) ||
      !data.GetString("data", &message_data)) {
    LOG(ERROR) << "Invalid extensionMessage.";
    return;
  }
  if (!IsConnected()) {
    return;
  }
  protocol::ExtensionMessage message;
  message.set_type(type);
  message.set_data(message_data);
  host_connection_->host_stub()->DeliverClientMessage(message);
}

void ChromotingInstance::HandleAllowMouseLockMessage() {
  input_handler_.AllowMouseLock();
}

void ChromotingInstance::HandleEnableMediaSourceRendering() {
  use_media_source_rendering_ = true;
}

ChromotingStats* ChromotingInstance::GetStats() {
  if (!video_renderer_.get())
    return NULL;
  return video_renderer_->GetStats();
}

void ChromotingInstance::PostChromotingMessage(const std::string& method,
                                               const pp::VarDictionary& data) {
  pp::VarDictionary message;
  message.Set(pp::Var("method"), pp::Var(method));
  message.Set(pp::Var("data"), data);
  PostMessage(message);
}

void ChromotingInstance::PostLegacyJsonMessage(
    const std::string& method,
    scoped_ptr<base::DictionaryValue> data) {
  scoped_ptr<base::DictionaryValue> message(new base::DictionaryValue());
  message->SetString("method", method);
  message->Set("data", data.release());

  std::string message_json;
  base::JSONWriter::Write(message.get(), &message_json);
  PostMessage(pp::Var(message_json));
}

void ChromotingInstance::SendTrappedKey(uint32 usb_keycode, bool pressed) {
  scoped_ptr<base::DictionaryValue> data(new base::DictionaryValue());
  data->SetInteger("usbKeycode", usb_keycode);
  data->SetBoolean("pressed", pressed);
  PostLegacyJsonMessage("trappedKeyEvent", data.Pass());
}

void ChromotingInstance::SendOutgoingIq(const std::string& iq) {
  scoped_ptr<base::DictionaryValue> data(new base::DictionaryValue());
  data->SetString("iq", iq);
  PostLegacyJsonMessage("sendOutgoingIq", data.Pass());
}

void ChromotingInstance::SendPerfStats() {
  if (!video_renderer_.get()) {
    return;
  }

  plugin_task_runner_->PostDelayedTask(
      FROM_HERE, base::Bind(&ChromotingInstance::SendPerfStats,
                            weak_factory_.GetWeakPtr()),
      base::TimeDelta::FromMilliseconds(kPerfStatsIntervalMs));

  scoped_ptr<base::DictionaryValue> data(new base::DictionaryValue());
  ChromotingStats* stats = video_renderer_->GetStats();
  data->SetDouble("videoBandwidth", stats->video_bandwidth()->Rate());
  data->SetDouble("videoFrameRate", stats->video_frame_rate()->Rate());
  data->SetDouble("captureLatency", stats->video_capture_ms()->Average());
  data->SetDouble("encodeLatency", stats->video_encode_ms()->Average());
  data->SetDouble("decodeLatency", stats->video_decode_ms()->Average());
  data->SetDouble("renderLatency", stats->video_paint_ms()->Average());
  data->SetDouble("roundtripLatency", stats->round_trip_ms()->Average());
  PostLegacyJsonMessage("onPerfStats", data.Pass());
}

// static
void ChromotingInstance::RegisterLogMessageHandler() {
  base::AutoLock lock(g_logging_lock.Get());

  VLOG(1) << "Registering global log handler";

  // Record previous handler so we can call it in a chain.
  g_logging_old_handler = logging::GetLogMessageHandler();

  // Set up log message handler.
  // This is not thread-safe so we need it within our lock.
  logging::SetLogMessageHandler(&LogToUI);
}

void ChromotingInstance::RegisterLoggingInstance() {
  base::AutoLock lock(g_logging_lock.Get());

  // Register this instance as the one that will handle all logging calls
  // and display them to the user.
  // If multiple plugins are run, then the last one registered will handle all
  // logging for all instances.
  g_logging_instance.Get() = weak_factory_.GetWeakPtr();
  g_logging_task_runner.Get() = plugin_task_runner_;
  g_has_logging_instance = true;
}

void ChromotingInstance::UnregisterLoggingInstance() {
  base::AutoLock lock(g_logging_lock.Get());

  // Don't unregister unless we're the currently registered instance.
  if (this != g_logging_instance.Get().get())
    return;

  // Unregister this instance for logging.
  g_has_logging_instance = false;
  g_logging_instance.Get().reset();
  g_logging_task_runner.Get() = NULL;

  VLOG(1) << "Unregistering global log handler";
}

// static
bool ChromotingInstance::LogToUI(int severity, const char* file, int line,
                                 size_t message_start,
                                 const std::string& str) {
  // Note that we're reading |g_has_logging_instance| outside of a lock.
  // This lockless read is done so that we don't needlessly slow down global
  // logging with a lock for each log message.
  //
  // This lockless read is safe because:
  //
  // Misreading a false value (when it should be true) means that we'll simply
  // skip processing a few log messages.
  //
  // Misreading a true value (when it should be false) means that we'll take
  // the lock and check |g_logging_instance| unnecessarily. This is not
  // problematic because we always set |g_logging_instance| inside a lock.
  if (g_has_logging_instance) {
    scoped_refptr<base::SingleThreadTaskRunner> logging_task_runner;
    base::WeakPtr<ChromotingInstance> logging_instance;

    {
      base::AutoLock lock(g_logging_lock.Get());
      // If we're on the logging thread and |g_logging_to_plugin| is set then
      // this LOG message came from handling a previous LOG message and we
      // should skip it to avoid an infinite loop of LOG messages.
      if (!g_logging_task_runner.Get()->BelongsToCurrentThread() ||
          !g_logging_to_plugin) {
        logging_task_runner = g_logging_task_runner.Get();
        logging_instance = g_logging_instance.Get();
      }
    }

    if (logging_task_runner.get()) {
      std::string message = remoting::GetTimestampString();
      message += (str.c_str() + message_start);

      logging_task_runner->PostTask(
          FROM_HERE, base::Bind(&ChromotingInstance::ProcessLogToUI,
                                logging_instance, message));
    }
  }

  if (g_logging_old_handler)
    return (g_logging_old_handler)(severity, file, line, message_start, str);
  return false;
}

void ChromotingInstance::ProcessLogToUI(const std::string& message) {
  DCHECK(plugin_task_runner_->BelongsToCurrentThread());

  // This flag (which is set only here) is used to prevent LogToUI from posting
  // new tasks while we're in the middle of servicing a LOG call. This can
  // happen if the call to LogDebugInfo tries to LOG anything.
  // Since it is read on the plugin thread, we don't need to lock to set it.
  g_logging_to_plugin = true;
  scoped_ptr<base::DictionaryValue> data(new base::DictionaryValue());
  data->SetString("message", message);
  PostLegacyJsonMessage("logDebugMessage", data.Pass());
  g_logging_to_plugin = false;
}

bool ChromotingInstance::IsCallerAppOrExtension() {
  const pp::URLUtil_Dev* url_util = pp::URLUtil_Dev::Get();
  if (!url_util)
    return false;

  PP_URLComponents_Dev url_components;
  pp::Var url_var = url_util->GetDocumentURL(this, &url_components);
  if (!url_var.is_string())
    return false;

  std::string url = url_var.AsString();
  std::string url_scheme = url.substr(url_components.scheme.begin,
                                      url_components.scheme.len);
  return url_scheme == kChromeExtensionUrlScheme;
}

bool ChromotingInstance::IsConnected() {
  return host_connection_.get() &&
    (host_connection_->state() == protocol::ConnectionToHost::CONNECTED);
}

void ChromotingInstance::OnMediaSourceSize(const webrtc::DesktopSize& size,
                                           const webrtc::DesktopVector& dpi) {
  SetDesktopSize(size, dpi);
}

void ChromotingInstance::OnMediaSourceShape(
    const webrtc::DesktopRegion& shape) {
  SetDesktopShape(shape);
}

void ChromotingInstance::OnMediaSourceReset(const std::string& format) {
  scoped_ptr<base::DictionaryValue> data(new base::DictionaryValue());
  data->SetString("format", format);
  PostLegacyJsonMessage("mediaSourceReset", data.Pass());
}

void ChromotingInstance::OnMediaSourceData(uint8_t* buffer,
                                           size_t buffer_size) {
  pp::VarArrayBuffer array_buffer(buffer_size);
  void* data_ptr = array_buffer.Map();
  memcpy(data_ptr, buffer, buffer_size);
  array_buffer.Unmap();
  pp::VarDictionary data_dictionary;
  data_dictionary.Set(pp::Var("buffer"), array_buffer);
  PostChromotingMessage("mediaSourceData", data_dictionary);
}

}  // namespace remoting
