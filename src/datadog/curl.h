#pragma once

// This component provides a `class`, `Curl`, that implements the `HTTPClient`
// interface in terms of [libcurl][1].  `class Curl` manages a thread that is
// used as the event loop for libcurl.
//
// If this library was built in a mode that does not include libcurl, then this
// file and its implementation, `curl.cpp`, will not be included.
//
// [1]: https://curl.se/libcurl/

#include <curl/curl.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "http_client.h"
#include "json_fwd.hpp"

namespace datadog {
namespace tracing {

// `class CurlLibrary` has one member function for every libcurl function used
// in the implementation of this component.
//
// The naming convention is that `CurlLibrary::foo_bar` corresponds to
// `curl_foo_bar`, with the exception of `curl_easy_getinfo` and
// `curl_easy_setopt`. `curl_easy_getinfo` and `curl_easy_setopt` have multiple
// corresponding member functions -- one for each `CURLINFO` value or
// `CURLoption` value, respectively.
//
// The default implementations forward to their libcurl counterparts.  Unit
// tests override some of the member functions.
class CurlLibrary {
 public:
  typedef size_t (*WriteCallback)(char *ptr, size_t size, size_t nmemb,
                                  void *userdata);
  typedef size_t (*HeaderCallback)(char *buffer, size_t size, size_t nitems,
                                   void *userdata);

  virtual ~CurlLibrary() = default;

  virtual void easy_cleanup(CURL *handle);
  virtual CURL *easy_init();
  virtual CURLcode easy_getinfo_private(CURL *curl, char **user_data);
  virtual CURLcode easy_getinfo_response_code(CURL *curl, long *code);
  virtual CURLcode easy_setopt_errorbuffer(CURL *handle, char *buffer);
  virtual CURLcode easy_setopt_headerdata(CURL *handle, void *data);
  virtual CURLcode easy_setopt_headerfunction(CURL *handle, HeaderCallback);
  virtual CURLcode easy_setopt_httpheader(CURL *handle, curl_slist *headers);
  virtual CURLcode easy_setopt_post(CURL *handle, long post);
  virtual CURLcode easy_setopt_postfields(CURL *handle, const char *data);
  virtual CURLcode easy_setopt_postfieldsize(CURL *handle, long size);
  virtual CURLcode easy_setopt_private(CURL *handle, void *pointer);
  virtual CURLcode easy_setopt_unix_socket_path(CURL *handle, const char *path);
  virtual CURLcode easy_setopt_url(CURL *handle, const char *url);
  virtual CURLcode easy_setopt_writedata(CURL *handle, void *data);
  virtual CURLcode easy_setopt_writefunction(CURL *handle, WriteCallback);
  virtual const char *easy_strerror(CURLcode error);
  virtual void global_cleanup();
  virtual CURLcode global_init(long flags);
  virtual CURLMcode multi_add_handle(CURLM *multi_handle, CURL *easy_handle);
  virtual CURLMcode multi_cleanup(CURLM *multi_handle);
  virtual CURLMsg *multi_info_read(CURLM *multi_handle, int *msgs_in_queue);
  virtual CURLM *multi_init();
  virtual CURLMcode multi_perform(CURLM *multi_handle, int *running_handles);
  virtual CURLMcode multi_poll(CURLM *multi_handle, curl_waitfd extra_fds[],
                               unsigned extra_nfds, int timeout_ms,
                               int *numfds);
  virtual CURLMcode multi_remove_handle(CURLM *multi_handle, CURL *easy_handle);
  virtual const char *multi_strerror(CURLMcode error);
  virtual CURLMcode multi_wakeup(CURLM *multi_handle);
  virtual curl_slist *slist_append(curl_slist *list, const char *string);
  virtual void slist_free_all(curl_slist *list);
};

// `class CurlEventLoop` is an interface over the different ways that libcurl
// can manage I/O. One implementation of `CurlEventLoop` might spawn
// a thread that polls an internal event loop for I/O events. Another
// implementation might use an external event loop, such as that provided by
// nginx, libev, libuv, or libevent.
// `class Curl` takes a `CurlEventLoop` in its constructor.
class CurlEventLoop {
 public:
  // Add the specified request `handle` to the event loop. Return an error if
  // the `handle` cannot be added. If the `handle` is successfully added,
  // register the specified `on_error` callback for when an error occurs in the
  // processing of the request before a full response is received, and register
  // the specified `on_done` callback for when a full response is received. If
  // this function indicates success, by returning `nullopt`, then exactly one
  // of either `on_error` or `on_done` will eventually be invoked.
  // The caller is responsible for freeing the `handle`. `handle` will be
  // removed from the event loop before one of `on_error` or `on_done` is
  // invoked. To remove the `handle` sooner, call `remove_handle`.
  //
  // Note that a `CurlEventLoop` implementation might use the private data
  // pointer property (`CURLINFO_PRIVATE`) of `handle` between when
  // `add_handle` is called and when one of the following occurs: either of
  // `on_error` or `on_done` is invoked, `add_handle` returns an error, or
  // `remove_handle` is called. Any existing private data pointer will be
  // restored afterward; but, other handlers registered with `handle`, such as
  // `CURLOPT_HEADERFUNCTION` and `CURLOPT_WRITEFUNCTION`, must not access
  // `handle`'s private data pointer. This restriction is to afford
  // implementations of `CurlEventLoop` with limited state.
  virtual Expected<void> add_handle(
      CURL *handle, std::function<void(CURLcode) /*noexcept*/> on_error,
      std::function<void() /*noexcept*/> on_done) = 0;

  // Remove the specified request `handle` from the event loop. Return an
  // error if one occurs.
  virtual Expected<void> remove_handle(CURL *handle) = 0;

  // Destroy this object. Any request handle that was previously added to the
  // event loop via `add_handle` will be removed as if by a call to
  // `remove_handle`.
  virtual ~CurlEventLoop() = default;

  // Wait until there are no more outstanding requests, or until the specified
  // `deadline`. An implementation is permitted to instead return immediately.
  virtual void drain(std::chrono::steady_clock::time_point deadline) = 0;
};

class CurlImpl;
class Logger;

class Curl : public HTTPClient {
  CurlImpl *impl_;

 public:
  using ThreadGenerator = std::function<std::thread(std::function<void()> &&)>;

  // Create a `Curl` instance that:
  //
  // - uses the specified `logger` to log diagnostics,
  // - uses the optionally specified `library` to access libcurl functions,
  //   or uses a default library if one is not specified,
  // - uses the optionally specified `event_loop` to dispatch curl handles, or
  //   uses a default event loop if one is not specified,
  // - and uses the optionally specified `make_thread` to spawn the event loop
  //   thread if the default even loop is to be used, or uses a default thread
  //   creation function if one is not specified.
  //
  // If `event_loop` is null, then the resulting `Curl` instance's member
  // functions return an error and have no other effect. The behavior is
  // undefined if `logger` is null or if `make_thread` contains no target.
  explicit Curl(const std::shared_ptr<Logger> &logger);
  Curl(const std::shared_ptr<Logger> &, CurlLibrary &library);
  Curl(const std::shared_ptr<Logger> &,
       const std::shared_ptr<CurlEventLoop> &event_loop);
  Curl(const std::shared_ptr<Logger> &, const std::shared_ptr<CurlEventLoop> &,
       CurlLibrary &);
  Curl(const std::shared_ptr<Logger> &,
       const Curl::ThreadGenerator &make_thread, CurlLibrary &);

  ~Curl() override;

  Curl(const Curl &) = delete;

  Expected<void> post(const URL &url, HeadersSetter set_headers,
                      std::string body, ResponseHandler on_response,
                      ErrorHandler on_error) override;

  void drain(std::chrono::steady_clock::time_point deadline) override;

  nlohmann::json config_json() const override;
};

}  // namespace tracing
}  // namespace datadog
