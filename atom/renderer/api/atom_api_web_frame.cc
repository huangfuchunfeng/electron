// Copyright (c) 2014 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "atom/renderer/api/atom_api_web_frame.h"

#include <utility>

#include "atom/common/api/api_messages.h"
#include "atom/common/api/event_emitter_caller.h"
#include "atom/common/native_mate_converters/blink_converter.h"
#include "atom/common/native_mate_converters/callback.h"
#include "atom/common/native_mate_converters/gfx_converter.h"
#include "atom/common/native_mate_converters/string16_converter.h"
#include "atom/renderer/api/atom_api_spell_check_client.h"
#include "base/memory/memory_pressure_listener.h"
#include "content/public/renderer/render_frame.h"
#include "content/public/renderer/render_frame_observer.h"
#include "content/public/renderer/render_frame_visitor.h"
#include "content/public/renderer/render_view.h"
#include "native_mate/dictionary.h"
#include "native_mate/object_template_builder.h"
#include "third_party/blink/public/platform/web_cache.h"
#include "third_party/blink/public/web/web_custom_element.h"
#include "third_party/blink/public/web/web_document.h"
#include "third_party/blink/public/web/web_element.h"
#include "third_party/blink/public/web/web_frame_widget.h"
#include "third_party/blink/public/web/web_ime_text_span.h"
#include "third_party/blink/public/web/web_input_method_controller.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/public/web/web_script_execution_callback.h"
#include "third_party/blink/public/web/web_script_source.h"
#include "third_party/blink/public/web/web_view.h"
#include "url/url_util.h"

#include "atom/common/node_includes.h"

namespace mate {

template <>
struct Converter<blink::WebLocalFrame::ScriptExecutionType> {
  static bool FromV8(v8::Isolate* isolate,
                     v8::Local<v8::Value> val,
                     blink::WebLocalFrame::ScriptExecutionType* out) {
    std::string execution_type;
    if (!ConvertFromV8(isolate, val, &execution_type))
      return false;
    if (execution_type == "asynchronous") {
      *out = blink::WebLocalFrame::kAsynchronous;
    } else if (execution_type == "asynchronousBlockingOnload") {
      *out = blink::WebLocalFrame::kAsynchronousBlockingOnload;
    } else if (execution_type == "synchronous") {
      *out = blink::WebLocalFrame::kSynchronous;
    } else {
      return false;
    }
    return true;
  }
};

}  // namespace mate

namespace atom {

namespace api {

namespace {

content::RenderFrame* GetRenderFrame(v8::Local<v8::Value> value) {
  v8::Local<v8::Context> context =
      v8::Local<v8::Object>::Cast(value)->CreationContext();
  if (context.IsEmpty())
    return nullptr;
  blink::WebLocalFrame* frame = blink::WebLocalFrame::FrameForContext(context);
  if (!frame)
    return nullptr;
  return content::RenderFrame::FromWebFrame(frame);
}

class RenderFrameStatus : public content::RenderFrameObserver {
 public:
  explicit RenderFrameStatus(content::RenderFrame* render_frame)
      : content::RenderFrameObserver(render_frame) {}
  ~RenderFrameStatus() final {}

  bool is_ok() { return render_frame() != nullptr; }

  // RenderFrameObserver implementation.
  void OnDestruct() final {}
};

class ScriptExecutionCallback : public blink::WebScriptExecutionCallback {
 public:
  using CompletionCallback =
      base::Callback<void(const v8::Local<v8::Value>& result)>;

  explicit ScriptExecutionCallback(const CompletionCallback& callback)
      : callback_(callback) {}
  ~ScriptExecutionCallback() override {}

  void Completed(
      const blink::WebVector<v8::Local<v8::Value>>& result) override {
    if (!callback_.is_null() && !result.IsEmpty() && !result[0].IsEmpty())
      // Right now only single results per frame is supported.
      callback_.Run(result[0]);
    delete this;
  }

 private:
  CompletionCallback callback_;

  DISALLOW_COPY_AND_ASSIGN(ScriptExecutionCallback);
};

class FrameSpellChecker : public content::RenderFrameVisitor {
 public:
  explicit FrameSpellChecker(SpellCheckClient* spell_check_client,
                             content::RenderFrame* main_frame)
      : spell_check_client_(spell_check_client), main_frame_(main_frame) {}
  ~FrameSpellChecker() override {
    spell_check_client_ = nullptr;
    main_frame_ = nullptr;
  }
  bool Visit(content::RenderFrame* render_frame) override {
    auto* view = render_frame->GetRenderView();
    if (view->GetMainRenderFrame() == main_frame_ ||
        (render_frame->IsMainFrame() && render_frame == main_frame_)) {
      render_frame->GetWebFrame()->SetTextCheckClient(spell_check_client_);
    }
    return true;
  }

 private:
  SpellCheckClient* spell_check_client_;
  content::RenderFrame* main_frame_;
  DISALLOW_COPY_AND_ASSIGN(FrameSpellChecker);
};

}  // namespace

class AtomWebFrameObserver : public content::RenderFrameObserver {
 public:
  explicit AtomWebFrameObserver(
      content::RenderFrame* render_frame,
      std::unique_ptr<SpellCheckClient> spell_check_client)
      : content::RenderFrameObserver(render_frame),
        spell_check_client_(std::move(spell_check_client)) {}
  ~AtomWebFrameObserver() final {}

  // RenderFrameObserver implementation.
  void OnDestruct() final {
    spell_check_client_.reset();
    // Frame observers should delete themselves
    delete this;
  }

 private:
  std::unique_ptr<SpellCheckClient> spell_check_client_;
};

WebFrame::WebFrame(v8::Isolate* isolate)
    : web_frame_(blink::WebLocalFrame::FrameForCurrentContext()) {
  Init(isolate);
}

WebFrame::WebFrame(v8::Isolate* isolate, blink::WebLocalFrame* blink_frame)
    : web_frame_(blink_frame) {
  Init(isolate);
}

WebFrame::~WebFrame() {}

void WebFrame::SetName(const std::string& name) {
  web_frame_->SetName(blink::WebString::FromUTF8(name));
}

double WebFrame::SetZoomLevel(double level) {
  double result = 0.0;
  content::RenderFrame* render_frame =
      content::RenderFrame::FromWebFrame(web_frame_);
  render_frame->Send(new AtomFrameHostMsg_SetTemporaryZoomLevel(
      render_frame->GetRoutingID(), level, &result));
  return result;
}

double WebFrame::GetZoomLevel() const {
  double result = 0.0;
  content::RenderFrame* render_frame =
      content::RenderFrame::FromWebFrame(web_frame_);
  render_frame->Send(
      new AtomFrameHostMsg_GetZoomLevel(render_frame->GetRoutingID(), &result));
  return result;
}

double WebFrame::SetZoomFactor(double factor) {
  return blink::WebView::ZoomLevelToZoomFactor(
      SetZoomLevel(blink::WebView::ZoomFactorToZoomLevel(factor)));
}

double WebFrame::GetZoomFactor() const {
  return blink::WebView::ZoomLevelToZoomFactor(GetZoomLevel());
}

void WebFrame::SetVisualZoomLevelLimits(double min_level, double max_level) {
  web_frame_->View()->SetDefaultPageScaleLimits(min_level, max_level);
  web_frame_->View()->SetIgnoreViewportTagScaleLimits(true);
}

void WebFrame::SetLayoutZoomLevelLimits(double min_level, double max_level) {
  web_frame_->View()->ZoomLimitsChanged(min_level, max_level);
}

void WebFrame::AllowGuestViewElementDefinition(
    v8::Local<v8::Object> context,
    v8::Local<v8::Function> register_cb) {
  v8::HandleScope handle_scope(isolate());
  v8::Context::Scope context_scope(context->CreationContext());
  blink::WebCustomElement::EmbedderNamesAllowedScope embedder_names_scope;
  web_frame_->RequestExecuteV8Function(context->CreationContext(), register_cb,
                                       v8::Null(isolate()), 0, nullptr,
                                       nullptr);
}

int WebFrame::GetWebFrameId(v8::Local<v8::Value> content_window) {
  // Get the WebLocalFrame before (possibly) executing any user-space JS while
  // getting the |params|. We track the status of the RenderFrame via an
  // observer in case it is deleted during user code execution.
  content::RenderFrame* render_frame = GetRenderFrame(content_window);
  RenderFrameStatus render_frame_status(render_frame);

  if (!render_frame_status.is_ok())
    return -1;

  blink::WebLocalFrame* frame = render_frame->GetWebFrame();
  // Parent must exist.
  blink::WebFrame* parent_frame = frame->Parent();
  DCHECK(parent_frame);
  DCHECK(parent_frame->IsWebLocalFrame());

  return render_frame->GetRoutingID();
}

void WebFrame::SetSpellCheckProvider(mate::Arguments* args,
                                     const std::string& language,
                                     v8::Local<v8::Object> provider) {
  auto context = args->isolate()->GetCurrentContext();
  if (!provider->Has(context, mate::StringToV8(args->isolate(), "spellCheck"))
           .ToChecked()) {
    args->ThrowError("\"spellCheck\" has to be defined");
    return;
  }

  auto spell_check_client =
      std::make_unique<SpellCheckClient>(language, args->isolate(), provider);
  // Set spellchecker for all live frames in the same process or
  // in the sandbox mode for all live sub frames to this WebFrame.
  auto* render_frame = content::RenderFrame::FromWebFrame(web_frame_);
  FrameSpellChecker spell_checker(spell_check_client.get(), render_frame);
  content::RenderFrame::ForEach(&spell_checker);
  web_frame_->SetSpellCheckPanelHostClient(spell_check_client.get());
  new AtomWebFrameObserver(render_frame, std::move(spell_check_client));
}

void WebFrame::InsertText(const std::string& text) {
  web_frame_->FrameWidget()->GetActiveWebInputMethodController()->CommitText(
      blink::WebString::FromUTF8(text),
      blink::WebVector<blink::WebImeTextSpan>(), blink::WebRange(), 0);
}

void WebFrame::InsertCSS(const std::string& css) {
  web_frame_->GetDocument().InsertStyleSheet(blink::WebString::FromUTF8(css));
}

void WebFrame::ExecuteJavaScript(const base::string16& code,
                                 mate::Arguments* args) {
  bool has_user_gesture = false;
  args->GetNext(&has_user_gesture);
  ScriptExecutionCallback::CompletionCallback completion_callback;
  args->GetNext(&completion_callback);
  std::unique_ptr<blink::WebScriptExecutionCallback> callback(
      new ScriptExecutionCallback(completion_callback));
  web_frame_->RequestExecuteScriptAndReturnValue(
      blink::WebScriptSource(blink::WebString::FromUTF16(code)),
      has_user_gesture, callback.release());
}

void WebFrame::ExecuteJavaScriptInIsolatedWorld(
    int world_id,
    const std::vector<mate::Dictionary>& scripts,
    mate::Arguments* args) {
  std::vector<blink::WebScriptSource> sources;

  for (const auto& script : scripts) {
    base::string16 code;
    base::string16 url;
    int start_line = 1;
    script.Get("url", &url);
    script.Get("startLine", &start_line);

    if (!script.Get("code", &code)) {
      args->ThrowError("Invalid 'code'");
      return;
    }

    sources.emplace_back(
        blink::WebScriptSource(blink::WebString::FromUTF16(code),
                               blink::WebURL(GURL(url)), start_line));
  }

  bool has_user_gesture = false;
  args->GetNext(&has_user_gesture);

  blink::WebLocalFrame::ScriptExecutionType scriptExecutionType =
      blink::WebLocalFrame::kSynchronous;
  args->GetNext(&scriptExecutionType);

  ScriptExecutionCallback::CompletionCallback completion_callback;
  args->GetNext(&completion_callback);
  std::unique_ptr<blink::WebScriptExecutionCallback> callback(
      new ScriptExecutionCallback(completion_callback));

  web_frame_->RequestExecuteScriptInIsolatedWorld(
      world_id, &sources.front(), sources.size(), has_user_gesture,
      scriptExecutionType, callback.release());
}

void WebFrame::SetIsolatedWorldSecurityOrigin(int world_id,
                                              const std::string& origin_url) {
  web_frame_->SetIsolatedWorldSecurityOrigin(
      world_id, blink::WebSecurityOrigin::CreateFromString(
                    blink::WebString::FromUTF8(origin_url)));
}

void WebFrame::SetIsolatedWorldContentSecurityPolicy(
    int world_id,
    const std::string& security_policy) {
  web_frame_->SetIsolatedWorldContentSecurityPolicy(
      world_id, blink::WebString::FromUTF8(security_policy));
}

void WebFrame::SetIsolatedWorldHumanReadableName(int world_id,
                                                 const std::string& name) {
  web_frame_->SetIsolatedWorldHumanReadableName(
      world_id, blink::WebString::FromUTF8(name));
}

// static
mate::Handle<WebFrame> WebFrame::Create(v8::Isolate* isolate) {
  return mate::CreateHandle(isolate, new WebFrame(isolate));
}

blink::WebCache::ResourceTypeStats WebFrame::GetResourceUsage(
    v8::Isolate* isolate) {
  blink::WebCache::ResourceTypeStats stats;
  blink::WebCache::GetResourceTypeStats(&stats);
  return stats;
}

void WebFrame::ClearCache(v8::Isolate* isolate) {
  isolate->IdleNotificationDeadline(0.5);
  blink::WebCache::Clear();
  base::MemoryPressureListener::NotifyMemoryPressure(
      base::MemoryPressureListener::MEMORY_PRESSURE_LEVEL_CRITICAL);
}

v8::Local<v8::Value> WebFrame::Opener() const {
  blink::WebFrame* frame = web_frame_->Opener();
  if (frame && frame->IsWebLocalFrame())
    return mate::CreateHandle(isolate(),
                              new WebFrame(isolate(), frame->ToWebLocalFrame()))
        .ToV8();
  else
    return v8::Null(isolate());
}

v8::Local<v8::Value> WebFrame::Parent() const {
  blink::WebFrame* frame = web_frame_->Parent();
  if (frame && frame->IsWebLocalFrame())
    return mate::CreateHandle(isolate(),
                              new WebFrame(isolate(), frame->ToWebLocalFrame()))
        .ToV8();
  else
    return v8::Null(isolate());
}

v8::Local<v8::Value> WebFrame::Top() const {
  blink::WebFrame* frame = web_frame_->Top();
  if (frame && frame->IsWebLocalFrame())
    return mate::CreateHandle(isolate(),
                              new WebFrame(isolate(), frame->ToWebLocalFrame()))
        .ToV8();
  else
    return v8::Null(isolate());
}

v8::Local<v8::Value> WebFrame::FirstChild() const {
  blink::WebFrame* frame = web_frame_->FirstChild();
  if (frame && frame->IsWebLocalFrame())
    return mate::CreateHandle(isolate(),
                              new WebFrame(isolate(), frame->ToWebLocalFrame()))
        .ToV8();
  else
    return v8::Null(isolate());
}

v8::Local<v8::Value> WebFrame::NextSibling() const {
  blink::WebFrame* frame = web_frame_->NextSibling();
  if (frame && frame->IsWebLocalFrame())
    return mate::CreateHandle(isolate(),
                              new WebFrame(isolate(), frame->ToWebLocalFrame()))
        .ToV8();
  else
    return v8::Null(isolate());
}

v8::Local<v8::Value> WebFrame::GetFrameForSelector(
    const std::string& selector) const {
  blink::WebElement element = web_frame_->GetDocument().QuerySelector(
      blink::WebString::FromUTF8(selector));
  blink::WebLocalFrame* element_frame =
      blink::WebLocalFrame::FromFrameOwnerElement(element);
  if (element_frame)
    return mate::CreateHandle(isolate(), new WebFrame(isolate(), element_frame))
        .ToV8();
  else
    return v8::Null(isolate());
}

v8::Local<v8::Value> WebFrame::FindFrameByName(const std::string& name) const {
  blink::WebLocalFrame* local_frame =
      web_frame_->FindFrameByName(blink::WebString::FromUTF8(name))
          ->ToWebLocalFrame();
  if (local_frame)
    return mate::CreateHandle(isolate(), new WebFrame(isolate(), local_frame))
        .ToV8();
  else
    return v8::Null(isolate());
}

v8::Local<v8::Value> WebFrame::FindFrameByRoutingId(int routing_id) const {
  content::RenderFrame* render_frame =
      content::RenderFrame::FromRoutingID(routing_id);
  blink::WebLocalFrame* local_frame = nullptr;
  if (render_frame)
    local_frame = render_frame->GetWebFrame();
  if (local_frame)
    return mate::CreateHandle(isolate(), new WebFrame(isolate(), local_frame))
        .ToV8();
  else
    return v8::Null(isolate());
}

v8::Local<v8::Value> WebFrame::RoutingId() const {
  int routing_id = content::RenderFrame::GetRoutingIdForWebFrame(web_frame_);
  return v8::Number::New(isolate(), routing_id);
}

// static
void WebFrame::BuildPrototype(v8::Isolate* isolate,
                              v8::Local<v8::FunctionTemplate> prototype) {
  prototype->SetClassName(mate::StringToV8(isolate, "WebFrame"));
  mate::ObjectTemplateBuilder(isolate, prototype->PrototypeTemplate())
      .SetMethod("setName", &WebFrame::SetName)
      .SetMethod("setZoomLevel", &WebFrame::SetZoomLevel)
      .SetMethod("getZoomLevel", &WebFrame::GetZoomLevel)
      .SetMethod("setZoomFactor", &WebFrame::SetZoomFactor)
      .SetMethod("getZoomFactor", &WebFrame::GetZoomFactor)
      .SetMethod("setVisualZoomLevelLimits",
                 &WebFrame::SetVisualZoomLevelLimits)
      .SetMethod("setLayoutZoomLevelLimits",
                 &WebFrame::SetLayoutZoomLevelLimits)
      .SetMethod("allowGuestViewElementDefinition",
                 &WebFrame::AllowGuestViewElementDefinition)
      .SetMethod("getWebFrameId", &WebFrame::GetWebFrameId)
      .SetMethod("setSpellCheckProvider", &WebFrame::SetSpellCheckProvider)
      .SetMethod("insertText", &WebFrame::InsertText)
      .SetMethod("insertCSS", &WebFrame::InsertCSS)
      .SetMethod("executeJavaScript", &WebFrame::ExecuteJavaScript)
      .SetMethod("executeJavaScriptInIsolatedWorld",
                 &WebFrame::ExecuteJavaScriptInIsolatedWorld)
      .SetMethod("setIsolatedWorldSecurityOrigin",
                 &WebFrame::SetIsolatedWorldSecurityOrigin)
      .SetMethod("setIsolatedWorldContentSecurityPolicy",
                 &WebFrame::SetIsolatedWorldContentSecurityPolicy)
      .SetMethod("setIsolatedWorldHumanReadableName",
                 &WebFrame::SetIsolatedWorldHumanReadableName)
      .SetMethod("getResourceUsage", &WebFrame::GetResourceUsage)
      .SetMethod("clearCache", &WebFrame::ClearCache)
      .SetMethod("getFrameForSelector", &WebFrame::GetFrameForSelector)
      .SetMethod("findFrameByName", &WebFrame::FindFrameByName)
      .SetProperty("opener", &WebFrame::Opener)
      .SetProperty("parent", &WebFrame::Parent)
      .SetProperty("top", &WebFrame::Top)
      .SetProperty("firstChild", &WebFrame::FirstChild)
      .SetProperty("nextSibling", &WebFrame::NextSibling)
      .SetProperty("routingId", &WebFrame::RoutingId)
      .SetMethod("findFrameByRoutingId", &WebFrame::FindFrameByRoutingId);
}

}  // namespace api

}  // namespace atom

namespace {

using atom::api::WebFrame;

void Initialize(v8::Local<v8::Object> exports,
                v8::Local<v8::Value> unused,
                v8::Local<v8::Context> context,
                void* priv) {
  v8::Isolate* isolate = context->GetIsolate();
  mate::Dictionary dict(isolate, exports);
  dict.Set("webFrame", WebFrame::Create(isolate));
  dict.Set(
      "WebFrame",
      WebFrame::GetConstructor(isolate)->GetFunction(context).ToLocalChecked());
}

}  // namespace

NODE_BUILTIN_MODULE_CONTEXT_AWARE(atom_renderer_web_frame, Initialize)
