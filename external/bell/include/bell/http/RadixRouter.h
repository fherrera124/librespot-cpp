#pragma once

#include <memory>
#include <optional>
#include <sstream>
#include <unordered_map>

#include "bell/http/Common.h"
#include "bell/io/MemoryStream.h"

namespace bell::http {
/**
 * @brief A radix tree based router for HTTP requests, used in the HTTP server
 *
 * The router supports parameterized routes, e.g. /users/:id, as well as catch-all routes, noted by '*'
 */
template <typename RequestHandler>
class RadixRouter {
 public:
  // The Params type is a map of parameter names to their values
  using Params = std::unordered_map<std::string, std::string>;

  /**
   * @brief Registers a handler for the specified route and method.
   *
   * @param method The HTTP method to register the handler for
   * @param route Route template for the handler, e.g. /users/:id or /users*'
   * @param value handler type to register
   */
  void insert(Method method, const std::string& route,
              const RequestHandler& value) {
    auto* currentNode = &root;

    // Split the route into parts
    std::stringstream ss(route);
    std::string part;
    while (std::getline(ss, part, '/')) {
      if (part[0] == ':') {
        currentNode->isParam = true;
        currentNode->paramName = part.substr(1);
        part = "";
      } else if (part[0] == '*') {
        currentNode->isCatchAll = true;
        currentNode->methodHandlers[method] = value;
        return;
      }
      if (!currentNode->children.count(part)) {
        currentNode->children[part] = std::make_unique<Node>();
      }
      currentNode = currentNode->children[part].get();
    }
    currentNode->methodHandlers[method] = value;
  }

  /**
   * @brief Find a handler for the specified parameters
   */
  std::optional<
      std::pair<RequestHandler, std::unordered_map<std::string, std::string>>>
  find(Method method, std::string_view route) {
    auto* currentNode = &root;
    Params params;

    // Split the route into parts
    io::IMemoryStream istr(reinterpret_cast<const std::byte*>(route.data()),
                           route.size());

    std::string part;
    while (std::getline(istr, part, '/')) {
      if (currentNode->children.count(part)) {
        currentNode = currentNode->children[part].get();
      } else if (currentNode->isParam) {
        params[currentNode->paramName] = part;
        if (currentNode->children.count("")) {
          currentNode = currentNode->children[""].get();
        } else {
          return std::nullopt;
        }
      } else if (currentNode->isCatchAll) {
        params["**"] = part;
        if (currentNode->methodHandlers.count(method)) {
          return {{currentNode->methodHandlers[method], params}};
        }
        return std::nullopt;

      } else {
        return std::nullopt;
      }
    }
    if (currentNode->methodHandlers.count(method)) {
      return {{currentNode->methodHandlers[method], params}};
    }
    return std::nullopt;
  }

 private:
  // Node in the radix tree, representing a part of the route
  struct Node {
    std::unordered_map<std::string, std::unique_ptr<Node>> children;
    std::unordered_map<http::Method, RequestHandler> methodHandlers;
    std::string paramName;

    bool isParam = false;
    bool isCatchAll = false;
  };

  // Root of the radix tree
  Node root{};
};

}  // namespace bell::http
