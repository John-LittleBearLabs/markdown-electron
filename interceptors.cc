#include "interceptors.h"

#include "content/public/browser/url_loader_request_interceptor.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/url_response_head.mojom.h"

#include <cmark.h>

namespace {
  class MarkdownInterceptor final
    : public content::URLLoaderRequestInterceptor {
    raw_ptr<network::mojom::URLLoaderFactory> loader_factory_;
    raw_ptr<network::mojom::NetworkContext> network_context_;
    // raw_ptr<PrefService> pref_svc_;

    void MaybeCreateLoader(network::ResourceRequest const&,
                          content::BrowserContext*,
                          LoaderCallback) override;

  public:
    MarkdownInterceptor(network::mojom::URLLoaderFactory* handles_http,
                network::mojom::NetworkContext*);
  };
}

void electron_spin::AddInterceptors(std::vector<Interceptor>& in_out,
                                    network::mojom::URLLoaderFactory* ulf,
                                    network::mojom::NetworkContext* nc) {
  in_out.push_back(std::make_unique<MarkdownInterceptor>(ulf, nc));
}

namespace {
  class Loader : public network::mojom::URLLoader {
      mojo::Remote<network::mojom::URLLoaderClient> client_;
      mojo::Receiver<network::mojom::URLLoader> receiver_{this};
      GURL from_url_;
      raw_ptr<network::mojom::URLLoaderFactory> lower_;
      std::unique_ptr<network::SimpleURLLoader> loader_;

      void FollowRedirect(
          std::vector<std::string> const& removed_headers,
          net::HttpRequestHeaders const& modified_headers,
          net::HttpRequestHeaders const& modified_cors_exempt_headers,
          std::optional<::GURL> const& new_url) override {}
      void SetPriority(net::RequestPriority priority,
                      int32_t intra_priority_value) override {}
      void PauseReadingBodyFromNet() override {}
      void ResumeReadingBodyFromNet() override {}

  public:
      Loader(raw_ptr<network::mojom::URLLoaderFactory> lower);
      ~Loader() override;
      // Loader(mojo::PendingRemote<network::mojom::URLLoaderClient>,GURL,raw_ptr<network::mojom::URLLoaderFactory> lower);
      void SendLowerRequest(
        network::ResourceRequest const&,
        mojo::PendingReceiver<network::mojom::URLLoader>,
        mojo::PendingRemote<network::mojom::URLLoaderClient>);
      void Respond(std::unique_ptr<std::string>);

      std::shared_ptr<Loader> me_;
  };

  MarkdownInterceptor::MarkdownInterceptor(
    network::mojom::URLLoaderFactory* handles_http,
    network::mojom::NetworkContext* network_context)
  : loader_factory_{handles_http}
  , network_context_{network_context}
  {}
  void MarkdownInterceptor::MaybeCreateLoader(
        network::ResourceRequest const& req,
        content::BrowserContext* context,
        LoaderCallback loader_callback) {
    // auto& state = InterRequestState::FromBrowserContext(context);
    // state.network_context(network_context_);
    if (req.url.path_piece().ends_with(".md")) {
      VLOG(1) << "Intercepting markdown document " << req.url.spec();
      auto loader = std::make_shared<Loader>(loader_factory_);
      loader->me_ = loader;
      std::move(loader_callback)
          .Run(base::BindOnce(&Loader::SendLowerRequest, loader));
    } else {
      std::move(loader_callback).Run({});
    }
  }

  Loader::Loader(raw_ptr<network::mojom::URLLoaderFactory> lower)
  : lower_{lower}
  {}
  Loader::~Loader() {}
  void Loader::SendLowerRequest(
        network::ResourceRequest const& r,
        mojo::PendingReceiver<network::mojom::URLLoader> receiver,
        mojo::PendingRemote<network::mojom::URLLoaderClient> client) {
    receiver_.Bind(std::move(receiver));
    client_.Bind(std::move(client));
    constexpr auto kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("ipfs_gateway_request", R"(
        semantics {
            sender: "Markdown (electron-spin) "
            description: "Requesting raw markdown"
            trigger: "A URL that ends in .md"
            data: "None"
            destination: WEBSITE
        }
        policy {
            cookies_allowed: YES
        }
      )");
    using L = network::SimpleURLLoader;
    auto req = std::make_unique<network::ResourceRequest>(r);
    req->trusted_params = {};
    loader_ = L::Create(
        std::move(req),
        kTrafficAnnotation,
        FROM_HERE
      );
    loader_->SetTimeoutDuration(base::Seconds(99));
    loader_->SetAllowHttpErrorResults(false);
    auto bound = base::BindOnce(&Loader::Respond, base::Unretained(this));
    loader_->DownloadToStringOfUnboundedSizeUntilCrashAndDie(lower_, std::move(bound));
  }
  void Loader::Respond(std::unique_ptr<std::string> r) {
    if (!r || r->empty()) {
        LOG(ERROR) << "No HTTP response for a markdown document.";
        return;
    }
    auto html = std::unique_ptr<char,void(*)(void*)>(
        cmark_markdown_to_html(r->c_str(), r->size(), CMARK_OPT_DEFAULT),
        ::free
        );
    if (!html) {
        LOG(ERROR) << "Markdown conversion failed.";
        return;
    }
    uint32_t len = std::strlen(html.get());
    mojo::ScopedDataPipeProducerHandle pipe_prod_ = {};
    mojo::ScopedDataPipeConsumerHandle pipe_cons_ = {};
    mojo::CreateDataPipe(len, pipe_prod_, pipe_cons_);
    auto head = network::mojom::URLResponseHead::New();
    head->charset = "utf-8";
    pipe_prod_->WriteData(html.get(), &len, MOJO_BEGIN_WRITE_DATA_FLAG_ALL_OR_NONE);
    client_->OnReceiveResponse(std::move(head), std::move(pipe_cons_), std::nullopt);
    client_->OnComplete(network::URLLoaderCompletionStatus{});
    me_.reset();
  }
}
