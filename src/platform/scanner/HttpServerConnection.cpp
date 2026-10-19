// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Request handling for the authenticated device API.

#define NOMINMAX

#include "platform/scanner/HttpServer.h"
#include "platform/scanner/HttpServerInternal.h"
#include "platform/scanner/HttpServerProtocolInternal.h"
#include "core/scanner/InventatoryScanProtocol.h"

#include <mutex>
#include <string>

#include "core/inventory/Inventory.h"

namespace inventatory {

using namespace std;
using namespace http_server_detail;

bool LocalHttpServer::serveConnection(NativeSocket clientSocket, string requestText) {
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
    // Take the credential lock, reserve the counter and take callback serialization as one step, so
    // rotation cannot slip between them. The callback runs without the credential lock; a nested
    // duplicate request is rejected by the reservation.
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
      // Error text can come from the callback, so bound it.
      const auto boundedError = error.size() > 256U ? error.substr(0, 256U) : error;
      const auto response = authenticatedResponseText(status, *counter, expectedToken,
                                                       statusResultJson(false, boundedError));
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
    {
      // Serialization covers only the callback. The reservation admits one request at a time and
      // the epoch guards rotation, so a slow client cannot block rotation during a send.
      unique_lock<mutex> callbackLock(callbackSerialMutex_);
      credentialLock.unlock();
      try {
        syncSucceeded = callback && callback(request, syncResponse, error);
      } catch (...) {
        // A throwing callback must become a retryable failure, not terminate the service.
        syncSucceeded = false;
        error = "Device sync service failed";
      }
    }
    if (!syncSucceeded) {
      releaseReplayCounter(*counter, credentialEpoch);
      if (error.empty()) error = "Device sync service unavailable";
      return reject(503, error);
    }
    // Commit the replay marker only after the callback succeeds. If saving it fails, sync is
    // disabled rather than risk replaying the side effect.
    if (!advanceReplayCounter(*counter, credentialEpoch)) {
      return reject(409, "Request completed but its replay marker could not be saved");
    }
    const auto responseBody = deviceSyncResponseJson(syncResponse);
    if (responseBody.size() > kInventatoryScanResponseBodyLimit) {
      // Response outside the Scan R1 contract: the device keeps its outbox event and retries.
      return reject(500, "Device sync response exceeds the scanner limit");
    }
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
