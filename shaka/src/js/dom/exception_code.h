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

#ifndef SHAKA_EMBEDDED_JS_DOM_EXCEPTION_CODE_H_
#define SHAKA_EMBEDDED_JS_DOM_EXCEPTION_CODE_H_

#include <functional>  // for std::hash

namespace shaka {

enum ExceptionCode {
  IndexSizeError,
  HierarchyRequestError,
  WrongDocumentError,
  InvalidCharacterError,
  NoModificationAllowedError,
  NotFoundError,
  NotSupportedError,
  InUseAttributeError,
  InvalidStateError,
  SyntaxError,
  InvalidModificationError,
  NamespaceError,
  InvalidAccessError,
  TypeMismatchError,
  SecurityError,
  NetworkError,
  AbortError,
  URLMismatchError,
  QuotaExceededError,
  TimeoutError,
  InvalidNodeTypeError,
  DataCloneError,
  EncodingError,
  NotReadableError,
  UnknownError,
  ConstraintError,
  DataError,
  TransactionInactiveError,
  ReadOnlyError,
  VersionError,
  OperationError,
  NotAllowedError,
  MaxExceptionCode,  // Must remain last.
};

}  // namespace shaka

namespace std {

// Enumerations are not hashable until C++14.
template <>
struct hash<shaka::ExceptionCode> : hash<int> {};

}  // namespace std

#endif  // SHAKA_EMBEDDED_JS_DOM_EXCEPTION_CODE_H_
