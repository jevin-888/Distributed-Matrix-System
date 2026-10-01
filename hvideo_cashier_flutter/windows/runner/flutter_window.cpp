#include <winsock2.h>
#include <iphlpapi.h>
#include <ws2tcpip.h>

#include "flutter_window.h"

#include <optional>
#include <shellapi.h>

#include "flutter/generated_plugin_registrant.h"

FlutterWindow::FlutterWindow(const flutter::DartProject& project)
    : project_(project) {}

FlutterWindow::~FlutterWindow() {}

bool FlutterWindow::OnCreate() {
  if (!Win32Window::OnCreate()) {
    return false;
  }

  RECT frame = GetClientArea();

  // The size here must match the window dimensions to avoid unnecessary surface
  // creation / destruction in the startup path.
  flutter_controller_ = std::make_unique<flutter::FlutterViewController>(
      frame.right - frame.left, frame.bottom - frame.top, project_);
  // Ensure that basic setup of the controller was successful.
  if (!flutter_controller_->engine() || !flutter_controller_->view()) {
    return false;
  }
  RegisterPlugins(flutter_controller_->engine());
  network_discovery_channel_ =
      std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
          flutter_controller_->engine()->messenger(),
          "distributed_playback_system/network_discovery",
          &flutter::StandardMethodCodec::GetInstance());
  network_discovery_channel_->SetMethodCallHandler(
      [](const flutter::MethodCall<flutter::EncodableValue>& call,
         std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
        if (call.method_name() != "ipv4Networks") {
          result->NotImplemented();
          return;
        }
        ULONG size = 16384;
        std::vector<unsigned char> buffer(size);
        ULONG status = ERROR_BUFFER_OVERFLOW;
        for (int attempt = 0; attempt < 3 && status == ERROR_BUFFER_OVERFLOW; ++attempt) {
          buffer.resize(size);
          status = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST |
              GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
              reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
        }
        if (status != NO_ERROR && status != ERROR_NO_DATA) {
          result->Error("network_interfaces", "Cannot enumerate IPv4 networks");
          return;
        }
        flutter::EncodableList networks;
        if (status == NO_ERROR) {
          for (auto adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
               adapter != nullptr; adapter = adapter->Next) {
            if (adapter->OperStatus != IfOperStatusUp ||
                adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            for (auto address = adapter->FirstUnicastAddress; address != nullptr;
                 address = address->Next) {
              if (address->Address.lpSockaddr == nullptr ||
                  address->Address.lpSockaddr->sa_family != AF_INET ||
                  address->OnLinkPrefixLength == 0 || address->OnLinkPrefixLength > 32) continue;
              const auto ipv4 = reinterpret_cast<sockaddr_in*>(address->Address.lpSockaddr);
              const auto host = ntohl(ipv4->sin_addr.s_addr);
              if ((host >> 24) == 127 || (host >> 16) == 0xa9fe || host == 0) continue;
              char text[INET_ADDRSTRLEN] = {};
              if (InetNtopA(AF_INET, &ipv4->sin_addr, text, sizeof(text)) == nullptr) continue;
              networks.emplace_back(flutter::EncodableMap{
                  {flutter::EncodableValue("address"), flutter::EncodableValue(text)},
                  {flutter::EncodableValue("prefixLength"),
                   flutter::EncodableValue(static_cast<int32_t>(address->OnLinkPrefixLength))}});
            }
          }
        }
        result->Success(flutter::EncodableValue(networks));
      });
  admin_console_channel_ =
      std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
          flutter_controller_->engine()->messenger(),
          "distributed_playback_system/admin_console",
          &flutter::StandardMethodCodec::GetInstance());
  admin_console_channel_->SetMethodCallHandler(
      [window = GetHandle()](
          const flutter::MethodCall<flutter::EncodableValue>& call,
          std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>>
              result) {
        if (call.method_name() != "open") {
          result->NotImplemented();
          return;
        }
        const auto* arguments =
            std::get_if<flutter::EncodableMap>(call.arguments());
        if (arguments == nullptr) {
          result->Error("invalid_arguments", "URL is required");
          return;
        }
        const auto url_entry = arguments->find(flutter::EncodableValue("url"));
        if (url_entry == arguments->end()) {
          result->Error("invalid_arguments", "URL is required");
          return;
        }
        const auto* url = std::get_if<std::string>(&url_entry->second);
        if (url == nullptr ||
            (url->rfind("http://", 0) != 0 &&
             url->rfind("https://", 0) != 0)) {
          result->Error("invalid_url", "Only HTTP and HTTPS URLs are allowed");
          return;
        }
        const int length = ::MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, url->data(),
            static_cast<int>(url->size()), nullptr, 0);
        if (length <= 0) {
          result->Success(flutter::EncodableValue(false));
          return;
        }
        std::wstring wide_url(static_cast<size_t>(length), L'\0');
        ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, url->data(),
                              static_cast<int>(url->size()), wide_url.data(),
                              length);
        const auto launch_result = reinterpret_cast<INT_PTR>(
            ::ShellExecuteW(window, L"open", wide_url.c_str(), nullptr,
                            nullptr, SW_SHOWNORMAL));
        result->Success(flutter::EncodableValue(launch_result > 32));
      });
  SetChildContent(flutter_controller_->view()->GetNativeWindow());

  flutter_controller_->engine()->SetNextFrameCallback([&]() {
    this->Show();
  });

  // Flutter can complete the first frame before the "show window" callback is
  // registered. The following call ensures a frame is pending to ensure the
  // window is shown. It is a no-op if the first frame hasn't completed yet.
  flutter_controller_->ForceRedraw();

  return true;
}

void FlutterWindow::OnDestroy() {
  network_discovery_channel_.reset();
  admin_console_channel_.reset();
  if (flutter_controller_) {
    flutter_controller_ = nullptr;
  }

  Win32Window::OnDestroy();
}

LRESULT
FlutterWindow::MessageHandler(HWND hwnd, UINT const message,
                              WPARAM const wparam,
                              LPARAM const lparam) noexcept {
  // Give Flutter, including plugins, an opportunity to handle window messages.
  if (flutter_controller_) {
    std::optional<LRESULT> result =
        flutter_controller_->HandleTopLevelWindowProc(hwnd, message, wparam,
                                                      lparam);
    if (result) {
      return *result;
    }
  }

  switch (message) {
    case WM_FONTCHANGE:
      flutter_controller_->engine()->ReloadSystemFonts();
      break;
  }

  return Win32Window::MessageHandler(hwnd, message, wparam, lparam);
}
