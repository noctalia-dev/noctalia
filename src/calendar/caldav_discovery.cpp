#include "calendar/caldav_discovery.h"

#include "core/log.h"
#include "net/http_client.h"
#include "net/uri.h"

#include <cctype>
#include <cstring>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <memory>
#include <string_view>

namespace calendar {

  namespace {
    constexpr Logger kLog("calendar-caldav-discovery");

    struct DiscoveryContext {
      HttpClient& http;
      std::string serverUrl;
      std::string username;
      std::shared_ptr<const security::SecureBuffer> password;
      bool allowRedirectAuth = false;
      std::function<void(bool ok, std::vector<CalDavCollection>)> callback;
    };

    std::string localName(const xmlNode* node) {
      if (node == nullptr || node->name == nullptr) {
        return {};
      }
      return reinterpret_cast<const char*>(node->name);
    }

    std::string nodeText(xmlNode* node) {
      xmlChar* content = xmlNodeGetContent(node);
      if (content == nullptr) {
        return {};
      }
      std::string out(reinterpret_cast<const char*>(content));
      xmlFree(content);
      return out;
    }

    xmlNode* firstChildElement(xmlNode* node, std::string_view name) {
      if (node == nullptr) {
        return nullptr;
      }
      for (xmlNode* child = node->children; child != nullptr; child = child->next) {
        if (child->type == XML_ELEMENT_NODE && localName(child) == name) {
          return child;
        }
      }
      return nullptr;
    }

    std::vector<xmlNode*> childElements(xmlNode* node, std::string_view name) {
      std::vector<xmlNode*> out;
      if (node == nullptr) {
        return out;
      }
      for (xmlNode* child = node->children; child != nullptr; child = child->next) {
        if (child->type == XML_ELEMENT_NODE && localName(child) == name) {
          out.push_back(child);
        }
      }
      return out;
    }

    std::string findNestedHref(xmlNode* node) {
      if (node == nullptr) {
        return {};
      }
      if (localName(node) == "href") {
        return nodeText(node);
      }
      for (xmlNode* child = node->children; child != nullptr; child = child->next) {
        if (child->type != XML_ELEMENT_NODE) {
          continue;
        }
        std::string href = findNestedHref(child);
        if (!href.empty()) {
          return href;
        }
      }
      return {};
    }

    std::string normalizeBase(std::string_view url) {
      std::string out(url);
      if (out.empty()) {
        return out;
      }
      if (out.back() != '/') {
        out.push_back('/');
      }
      return out;
    }

    std::string originOf(std::string_view url) {
      const std::size_t scheme = url.find("://");
      if (scheme == std::string_view::npos) {
        return {};
      }
      const std::size_t hostStart = scheme + 3;
      const std::size_t pathStart = url.find('/', hostStart);
      if (pathStart == std::string_view::npos) {
        return std::string(url);
      }
      return std::string(url.substr(0, pathStart));
    }

    std::string requestBaseUrl(std::string_view requestedUrl, std::string_view effectiveUrl) {
      return !effectiveUrl.empty() ? std::string(effectiveUrl) : std::string(requestedUrl);
    }

    std::string resolveHref(std::string_view baseUrl, std::string_view href) {
      if (href.starts_with("https://") || href.starts_with("http://")) {
        return std::string(href);
      }
      if (href.empty()) {
        return {};
      }
      if (href.front() == '/') {
        const std::string origin = originOf(baseUrl);
        return origin.empty() ? std::string(href) : origin + std::string(href);
      }
      return normalizeBase(baseUrl) + std::string(href);
    }

    std::string collectionIdFromUrl(std::string_view url) {
      std::string_view trimmed(url);
      while (!trimmed.empty() && trimmed.back() == '/') {
        trimmed.remove_suffix(1);
      }
      const std::size_t slash = trimmed.rfind('/');
      std::string_view tail = slash == std::string_view::npos ? trimmed : trimmed.substr(slash + 1);
      std::string id = uri::decodeComponent(tail);
      std::erase_if(id, [](unsigned char c) { return std::isspace(c) != 0; });
      return id;
    }

    std::string firstPropertyText(xmlNode* response, std::string_view name) {
      for (xmlNode* propstat : childElements(response, "propstat")) {
        xmlNode* prop = firstChildElement(propstat, "prop");
        if (xmlNode* item = firstChildElement(prop, name)) {
          return nodeText(item);
        }
      }
      return {};
    }

    std::string firstChildText(xmlNode* node, std::string_view name) { return nodeText(firstChildElement(node, name)); }

    xmlNode* firstProperty(xmlNode* response, std::string_view name) {
      for (xmlNode* propstat : childElements(response, "propstat")) {
        xmlNode* prop = firstChildElement(propstat, "prop");
        if (xmlNode* item = firstChildElement(prop, name)) {
          return item;
        }
      }
      return nullptr;
    }

    bool propertyHasChild(xmlNode* response, std::string_view propertyName, std::string_view childName) {
      if (xmlNode* prop = firstProperty(response, propertyName)) {
        return firstChildElement(prop, childName) != nullptr;
      }
      return false;
    }

    bool supportsVEvent(xmlNode* response) {
      xmlNode* supported = firstProperty(response, "supported-calendar-component-set");
      if (supported == nullptr) {
        return true;
      }
      for (xmlNode* comp : childElements(supported, "comp")) {
        xmlChar* attr = xmlGetProp(comp, reinterpret_cast<const xmlChar*>("name"));
        if (attr == nullptr) {
          continue;
        }
        const bool isEvent = std::strcmp(reinterpret_cast<const char*>(attr), "VEVENT") == 0;
        xmlFree(attr);
        if (isEvent) {
          return true;
        }
      }
      return false;
    }

    std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> parseXml(const std::string& body) {
      return {
          xmlReadMemory(
              body.data(), static_cast<int>(body.size()), "caldav-discovery.xml", nullptr,
              XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING | XML_PARSE_RECOVER
          ),
          xmlFreeDoc
      };
    }

    std::string firstResponsePropertyHref(const std::string& body, std::string_view propertyName) {
      auto doc = parseXml(body);
      if (!doc) {
        return {};
      }
      xmlNode* root = xmlDocGetRootElement(doc.get());
      if (root == nullptr) {
        return {};
      }
      for (xmlNode* response : childElements(root, "response")) {
        if (xmlNode* prop = firstProperty(response, propertyName)) {
          return findNestedHref(prop);
        }
      }
      return {};
    }

    std::vector<CalDavCollection> parseCollections(const std::string& body, std::string_view homeUrl) {
      std::vector<CalDavCollection> out;
      auto doc = parseXml(body);
      if (!doc) {
        return out;
      }
      xmlNode* root = xmlDocGetRootElement(doc.get());
      if (root == nullptr) {
        return out;
      }

      for (xmlNode* response : childElements(root, "response")) {
        if (!propertyHasChild(response, "resourcetype", "calendar") || !supportsVEvent(response)) {
          continue;
        }
        const std::string href = firstChildText(response, "href");
        const std::string url = resolveHref(homeUrl, href);
        if (url.empty()) {
          continue;
        }

        CalDavCollection collection;
        collection.url = url;
        collection.id = collectionIdFromUrl(url);
        collection.name = firstPropertyText(response, "displayname");
        collection.color = firstPropertyText(response, "calendar-color");
        if (collection.name.empty()) {
          collection.name = collection.id;
        }
        if (!collection.id.empty()) {
          out.push_back(std::move(collection));
        }
      }
      return out;
    }

    HttpRequest propfindRequest(
        const std::string& url, const std::string& username,
        const std::shared_ptr<const security::SecureBuffer>& password, bool allowRedirectAuth, std::string body,
        int depth
    ) {
      HttpRequest req;
      req.method = "PROPFIND";
      req.url = url;
      req.body = std::move(body);
      req.followRedirects = true;
      req.allowRedirectAuth = allowRedirectAuth;
      req.basicUsername = username;
      if (password != nullptr) {
        const auto bytes = password->bytes();
        req.basicPassword = std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
      }
      req.headers = {
          "Depth: " + std::to_string(depth),
          "Content-Type: application/xml; charset=utf-8",
      };
      return req;
    }

    std::string currentUserPrincipalBody() {
      return "<?xml version=\"1.0\" encoding=\"utf-8\" ?>"
             "<D:propfind xmlns:D=\"DAV:\"><D:prop><D:current-user-principal/></D:prop></D:propfind>";
    }

    std::string calendarHomeSetBody() {
      return "<?xml version=\"1.0\" encoding=\"utf-8\" ?>"
             "<D:propfind xmlns:D=\"DAV:\" xmlns:C=\"urn:ietf:params:xml:ns:caldav\">"
             "<D:prop><C:calendar-home-set/></D:prop></D:propfind>";
    }

    std::string calendarsBody() {
      return "<?xml version=\"1.0\" encoding=\"utf-8\" ?>"
             "<D:propfind xmlns:D=\"DAV:\" xmlns:C=\"urn:ietf:params:xml:ns:caldav\" "
             "xmlns:CS=\"http://calendarserver.org/ns/\">"
             "<D:prop><D:resourcetype/><D:displayname/><CS:calendar-color/>"
             "<C:supported-calendar-component-set/></D:prop></D:propfind>";
    }

    void finish(std::shared_ptr<DiscoveryContext> ctx, bool ok, std::vector<CalDavCollection> collections = {}) {
      if (ctx->callback) {
        ctx->callback(ok, std::move(collections));
      }
    }

    void discoverCalendarHome(std::shared_ptr<DiscoveryContext> ctx, const std::string& principalUrl);
    void discoverCollections(std::shared_ptr<DiscoveryContext> ctx, const std::string& homeUrl);

    void discoverPrincipal(std::shared_ptr<DiscoveryContext> ctx) {
      ctx->http.request(
          propfindRequest(
              ctx->serverUrl, ctx->username, ctx->password, ctx->allowRedirectAuth, currentUserPrincipalBody(), 0
          ),
          [ctx](HttpResponse resp) {
            if (!resp.transportOk || (resp.status != 207 && resp.status != 200)) {
              kLog.warn("principal discovery failed http={}", resp.status);
              finish(ctx, false);
              return;
            }
            const std::string href = firstResponsePropertyHref(resp.body, "current-user-principal");
            const std::string principalUrl = resolveHref(requestBaseUrl(ctx->serverUrl, resp.effectiveUrl), href);
            if (principalUrl.empty()) {
              kLog.warn("principal discovery did not return current-user-principal");
              finish(ctx, false);
              return;
            }
            discoverCalendarHome(ctx, principalUrl);
          }
      );
    }

    void discoverCalendarHome(std::shared_ptr<DiscoveryContext> ctx, const std::string& principalUrl) {
      ctx->http.request(
          propfindRequest(principalUrl, ctx->username, ctx->password, ctx->allowRedirectAuth, calendarHomeSetBody(), 0),
          [ctx, principalUrl](HttpResponse resp) {
            if (!resp.transportOk || (resp.status != 207 && resp.status != 200)) {
              kLog.warn("calendar-home-set discovery failed http={}", resp.status);
              finish(ctx, false);
              return;
            }
            const std::string href = firstResponsePropertyHref(resp.body, "calendar-home-set");
            const std::string homeUrl = resolveHref(requestBaseUrl(principalUrl, resp.effectiveUrl), href);
            if (homeUrl.empty()) {
              kLog.warn("calendar-home-set discovery returned no href");
              finish(ctx, false);
              return;
            }
            discoverCollections(ctx, homeUrl);
          }
      );
    }

    void discoverCollections(std::shared_ptr<DiscoveryContext> ctx, const std::string& homeUrl) {
      ctx->http.request(
          propfindRequest(homeUrl, ctx->username, ctx->password, ctx->allowRedirectAuth, calendarsBody(), 1),
          [ctx, homeUrl](HttpResponse resp) {
            if (!resp.transportOk || (resp.status != 207 && resp.status != 200)) {
              kLog.warn("calendar collection discovery failed http={}", resp.status);
              finish(ctx, false);
              return;
            }
            std::vector<CalDavCollection> collections = parseCollections(resp.body, homeUrl);
            if (collections.empty()) {
              kLog.warn("calendar collection discovery returned no VEVENT collections");
              finish(ctx, false);
              return;
            }
            finish(ctx, true, std::move(collections));
          }
      );
    }
  } // namespace

  void discoverCalDavCollections(
      HttpClient& http, const std::string& serverUrl, const std::string& username,
      std::shared_ptr<const security::SecureBuffer> password, bool allowRedirectAuth,
      std::function<void(bool ok, std::vector<CalDavCollection>)> cb
  ) {
    auto ctx = std::make_shared<DiscoveryContext>(DiscoveryContext{
        http, normalizeBase(serverUrl), username, std::move(password), allowRedirectAuth, std::move(cb)
    });
    if (ctx->serverUrl.empty() || ctx->username.empty() || ctx->password == nullptr || ctx->password->empty()) {
      kLog.warn("missing server_url/username/password");
      finish(ctx, false);
      return;
    }
    discoverPrincipal(std::move(ctx));
  }

} // namespace calendar
