// Copyright 2016 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "src/js/dom/document.h"

#include "src/js/dom/comment.h"
#include "src/js/dom/element.h"
#include "src/js/dom/attr.h"
#include "src/js/dom/text.h"
#include "src/js/mse/video_element.h"
#include "src/memory/heap_tracer.h"
#include "src/util/clock.h"

namespace shaka {
namespace js {
namespace dom {

std::atomic<Document*> Document::instance_{nullptr};

Document::Document()
    : ContainerNode(DOCUMENT_NODE, nullptr),
      created_at_(util::Clock::Instance.GetMonotonicTime()) {}

// \cond Doxygen_Skip
Document::~Document() {
  if (instance_ == this)
    instance_ = nullptr;
}
// \endcond Doxygen_Skip

// static
Document* Document::CreateGlobalDocument() {
  DCHECK(instance_ == nullptr);
  return (instance_ = new Document());
}

std::string Document::node_name() const {
  return "#document";
}

optional<std::string> Document::NodeValue() const {
  return nullopt;
}

optional<std::string> Document::TextContent() const {
  return nullopt;
}

RefPtr<Element> Document::DocumentElement() const {
  for (auto& child : child_nodes()) {
    if (child->is_element())
      return static_cast<Element*>(child.get());
  }
  return nullptr;
}

RefPtr<Attr> Document::CreateAttribute(const std::string& name) {
  return new Attr(this, nullptr, util::ToAsciiLower(name), nullopt, nullopt, "");
}

RefPtr<Attr> Document::CreateAttributeNS(const std::string& namespace_uri, const std::string& name) {
  return new Attr(this, nullptr, name, namespace_uri, nullopt, "");
}

RefPtr<Element> Document::CreateElement(const std::string& name) {
  if (name == "video") {
    // This should only be used in Shaka Player integration tests.
    return new mse::HTMLVideoElement(this);
  }
  return new Element(this, name, nullopt, nullopt);
}

RefPtr<Comment> Document::CreateComment(const std::string& data) {
  return new Comment(this, data);
}

RefPtr<Text> Document::CreateTextNode(const std::string& data) {
  return new Text(this, data);
}


DocumentFactory::DocumentFactory() {
  AddMemberFunction("createAttribute", &Document::createAttribute);
  AddMemberFunction("createAttributeNS", &Document::createAttributeNS);
  AddMemberFunction("createElement", &Document::CreateElement);
  AddMemberFunction("createComment", &Document::CreateComment);
  AddMemberFunction("createTextNode", &Document::CreateTextNode);

  AddGenericProperty("documentElement", &Document::DocumentElement);

  // TODO: Consider adding createEvent.  Shaka Player only uses it in the
  // Microsoft EME polyfill and the unit tests.
  NotImplemented("createEvent");

  NotImplemented("createElementNS");
  NotImplemented("createDocumentFragment");
  NotImplemented("createCDATASection");
  NotImplemented("createProcessingInstruction");

  NotImplemented("createRange");
  NotImplemented("createNodeIterator");
  NotImplemented("createTreeWalker");

  NotImplemented("importNode");
  NotImplemented("adoptNode");
}

}  // namespace dom
}  // namespace js
}  // namespace shaka
