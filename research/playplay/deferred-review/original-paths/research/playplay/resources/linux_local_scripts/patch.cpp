            auto ptrRes = httpRes->bytesPtr();
            auto lenRes = httpRes->bytesLength();
            if (ptrRes && lenRes) {
                std::string resBody(reinterpret_cast<const char*>(ptrRes.value()), lenRes.value());
