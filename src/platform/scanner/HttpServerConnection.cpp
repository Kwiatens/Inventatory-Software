// Inventatory - Authenticated HTTP connection request handling.

#define NOMINMAX

#include "platform/scanner/HttpServer.h"
#include "platform/scanner/HttpServerInternal.h"
#include "platform/scanner/HttpServerProtocolInternal.h"

#include <mutex>
#include <string>

#include "core/inventory/Inventory.h"

namespace inventatory {

using namespace std;
using namespace http_server_detail;

bool LocalHttpServer::serveConnection(SOCKET clientSocket, string requestText) {
  // Parse the first request line and route only the tiny local API surface.
  const auto headerEnd = requestText.find("\r\n\r\n");
  if (headerEnd == string::npos) {
    return false;
  }
  if (headerEnd > kMaxHttpHeaderBytes) {
    const auto response = responseText("413 Payload Too Large", "text/plain; charset=utf-8", "Request too large");
    sendAll(clientSocket, response);
    return false;
  }

  const auto headers = requestText.substr(0, headerEnd);
  const auto body = requestText.substr(headerEnd + 4);
  string method;
  string target;
  string version;
  HttpHeaderMap headerValues;
  size_t declaredBodySize = 0;
  if (!parseHttpHeaders(headers, method, target, version, headerValues) ||
      !parseContentLength(headerValues, declaredBodySize) ||
      headerValues.find("transfer-encoding") != headerValues.end()) {
    const auto response = responseText("400 Bad Request", "text/plain; charset=utf-8", "Invalid request body");
    sendAll(clientSocket, response);
    return false;
  }
  if (declaredBodySize > kMaxHttpBodyBytes) {
    const auto response = responseText("413 Payload Too Large", "text/plain; charset=utf-8", "Request body too large");
    sendAll(clientSocket, response);
    return false;
  }
  if (body.size() != declaredBodySize) {
    const auto response = responseText("400 Bad Request", "text/plain; charset=utf-8", "Invalid request body");
    sendAll(clientSocket, response);
    return false;
  }

  if (method == "POST" && target == "/api/v1/device/sync") {
    // Capture credentials, reserve the counter, and acquire callback
    // serialization as one operation. Holding callbackSerialMutex_ while the
    // credential lock is released prevents rotation from crossing the gap
    // between reservation and callback. The callback/replay path then runs
    // without the credential lock, so an application callback can issue a
    // nested duplicate request and have it rejected by the reservation check.
    unique_lock<mutex> credentialLock(credentialOperationMutex_);
    const auto credentialEpoch = credentialEpoch_;
    string expectedDevice;
    string expectedToken;
    {
      lock_guard<mutex> lock(stateMutex_);
      expectedDevice = pairedDeviceId_;
      expectedToken = deviceToken_;
    }
    const auto header = [&headerValues](const char* name) {
      const auto found = headerValues.find(name);
      return found == headerValues.end() ? string{} : found->second;
    };
    const auto deviceId = header("x-inventatory-device");
    const auto counter = headerCounter(headerValues);
    const auto suppliedMac = header("x-inventatory-mac");
    const auto protocol = header("x-inventatory-protocol");
    if (protocol != to_string(kInventatoryScanTransportProtocolVersion) || deviceId.empty() || !counter ||
        !tokensMatch(deviceRequestMac(expectedToken, method, target, deviceId, *counter, body), suppliedMac)) {
      const auto response = responseText("401 Unauthorized", "application/json; charset=utf-8",
                                         statusResultJson(false, "Unauthorized device"));
      sendAll(clientSocket, response);
      return false;
    }
    const auto reject = [&](int status, const string& error) {
      const auto response = authenticatedResponseText(status, *counter, expectedToken, statusResultJson(false, error));
      sendAll(clientSocket, response);
      return false;
    };
    if (!expectedDevice.empty() && deviceId != expectedDevice) {
      return reject(400, "Device identity does not match the pairing");
    }
    DeviceSyncRequest request;
    string error;
    if (!parseDeviceSyncRequestJson(body, request, error) || request.deviceId != deviceId) {
      return reject(error == "Unsupported protocol version" ? 426 : 400,
                    error.empty() ? "Device identity does not match the transport envelope" : error);
    }
    if (!reserveReplayCounter(*counter, credentialEpoch)) {
      return reject(409, "Replayed or unavailable request counter");
    }
    DeviceSyncResponse syncResponse;
    bool syncSucceeded = false;
    SyncCallback callback;
    {
      lock_guard<mutex> lock(callbackMutex_);
      callback = onSync_;
    }
    unique_lock<mutex> callbackLock(callbackSerialMutex_);
    credentialLock.unlock();
    try {
      syncSucceeded = callback && callback(request, syncResponse, error);
    } catch (...) {
      // A malformed or unavailable application callback must become a
      // retryable transport failure; a worker exception must never terminate
      // the service process while a replay reservation is held.
      syncSucceeded = false;
      error = "Device sync service failed";
    }
    if (!syncSucceeded) {
      releaseReplayCounter(*counter, credentialEpoch);
      if (error.empty()) error = "Device sync service unavailable";
      return reject(503, error);
    }
    // Commit the replay marker only after the durable application callback has
    // succeeded. A callback failure releases the reservation so the R1 can retry;
    // marker persistence failure instead disables sync to avoid replaying a side effect.
    if (!advanceReplayCounter(*counter, credentialEpoch)) {
      return reject(409, "Request completed but its replay marker could not be saved");
    }
    const auto responseBody = deviceSyncResponseJson(syncResponse);
    const auto response = authenticatedResponseText(200, *counter, expectedToken, responseBody);
    sendAll(clientSocket, response);
    return true;
  }

  if (target.rfind("/api/", 0) == 0) {
    const auto response = responseText("404 Not Found", "application/json; charset=utf-8",
                                       statusResultJson(false, "Unsupported device API route"));
    sendAll(clientSocket, response);
    return false;
  }

  const auto response = responseText("404 Not Found", "text/plain; charset=utf-8", "Not found");
  sendAll(clientSocket, response);
  return false;
}

}  // namespace inventatory
