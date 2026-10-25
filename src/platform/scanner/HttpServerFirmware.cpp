// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Device API for Scan R1 firmware offers. The scanner pulls a checked package in chunks.

#define NOMINMAX

#include "platform/scanner/HttpServer.h"
#include "platform/scanner/HttpServerProtocolInternal.h"
#include "platform/scanner/ScanFirmwareCourier.h"
#include "core/scanner/InventatoryScanFirmware.h"

#include <mutex>
#include <string>

namespace inventatory {

using namespace std;
using namespace http_server_detail;

namespace {

constexpr char kFirmwareRoutePrefix[] = "/api/v1/device/firmware/";
constexpr char kJsonContentType[] = "application/json; charset=utf-8";
constexpr char kBinaryContentType[] = "application/octet-stream";

}  // namespace

void LocalHttpServer::setFirmwareCourier(ScanFirmwareCourier* courier) { firmwareCourier_ = courier; }

bool LocalHttpServer::serveFirmwareRequest(NativeSocket clientSocket, const string& target,
                                           const HttpHeaderMap& headerValues, const string& body) {
  // Same authentication as the sync route: the MAC covers the method, path, device, counter and exact
  // body, and the counter is reserved before the answer is built. Only the answers differ.
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
  // An empty token would make the MAC computable by anyone, so it never authenticates.
  if (expectedToken.empty() || protocol != to_string(kInventatoryScanTransportProtocolVersion) ||
      deviceId.empty() || !counter ||
      !tokensMatch(deviceRequestMac(expectedToken, "POST", target, deviceId, *counter, body), suppliedMac)) {
    const auto response = responseText("401 Unauthorized", kJsonContentType,
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

  // Parse before the counter is reserved, as the sync route does, so a malformed request spends nothing.
  const auto route = target.substr(sizeof(kFirmwareRoutePrefix) - 1U);
  ScanFirmwareOfferRequest offerRequest;
  ScanFirmwareChunkRequest chunkRequest;
  ScanFirmwareStatusReport statusReport;
  string error;
  if (route == "offer") {
    if (!parseScanFirmwareOfferJson(body, offerRequest, error)) return reject(400, error);
  } else if (route == "chunk") {
    if (!parseScanFirmwareChunkJson(body, chunkRequest, error)) return reject(400, error);
  } else if (route == "status") {
    if (!parseScanFirmwareStatusJson(body, statusReport, error)) return reject(400, error);
  } else {
    return reject(404, "Unsupported device API route");
  }
  if (firmwareCourier_ == nullptr) return reject(503, "Firmware service is unavailable");

  if (!reserveReplayCounter(*counter, credentialEpoch)) {
    return reject(409, "Replayed or unavailable request counter");
  }
  credentialLock.unlock();

  // Every authenticated request spends its counter, including a refused chunk, so a counter is never
  // accepted twice.
  int status = 200;
  string responseBody;
  string contentType = kJsonContentType;
  if (route == "offer") {
    status = firmwareCourier_->offer(offerRequest, responseBody);
  } else if (route == "chunk") {
    status = firmwareCourier_->chunk(chunkRequest, responseBody);
    if (status == 200) contentType = kBinaryContentType;
  } else {
    firmwareCourier_->recordStatus(statusReport);
    responseBody = "{}";
  }
  if (!advanceReplayCounter(*counter, credentialEpoch)) {
    return reject(409, "Request completed but its replay marker could not be saved");
  }
  const auto response = authenticatedResponseText(status, *counter, expectedToken, responseBody, contentType);
  sendAll(clientSocket, response);
  return true;
}

}  // namespace inventatory
