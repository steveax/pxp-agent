#include <pxp-agent/util/bolt_helpers.hpp>
#include <pxp-agent/configuration.hpp>
#include <pxp-agent/module.hpp>

#include <boost/algorithm/hex.hpp>

#define LEATHERMAN_LOGGING_NAMESPACE "puppetlabs.pxp_agent.util.bolt_helpers"
#include <leatherman/logging/logging.hpp>

#include <openssl/evp.h>

namespace fs       = boost::filesystem;
namespace alg      = boost::algorithm;
namespace lth_curl = leatherman::curl;
namespace lth_jc   = leatherman::json_container;
namespace lth_loc  = leatherman::locale;

namespace PXPAgent {
namespace Util {


    // NIX_DIR_PERMS is defined in pxp-agent/configuration
    #define NIX_TASK_FILE_PERMS NIX_DIR_PERMS

    // Downloads a file if it does not already exist on the filesystem. A check is made
    // on the filesystem to determine if the file at destination already exists and if
    // it already matches the sha256 provided with the file. If the file already exists
    // the function immediately returns.
    //
    // If the file does not exist attempt to download with leatherman.curl. Once the
    // download finishes a sha256 check occurs to ensure file contents are correct. Then
    // the file is moved to destination with boost::filesystem::rename.
    fs::path downloadFileFromMaster(const std::vector<std::string>& master_uris,
                                uint32_t connect_timeout,
                                uint32_t timeout,
                                lth_curl::client& client,
                                const fs::path& cache_dir,
                                const fs::path& destination,
                                const lth_jc::JsonContainer& file) {
        auto filename = fs::path(file.get<std::string>("filename")).filename();
        auto sha256 = file.get<std::string>("sha256");

        if (fs::exists(destination) && sha256 == calculateSha256(destination.string())) {
            fs::permissions(destination, NIX_TASK_FILE_PERMS);
            return destination;
        }

        if (master_uris.empty()) {
            throw Module::ProcessingError(lth_loc::format("Cannot download task. No master-uris were provided"));
        }

        auto tempname = cache_dir / fs::unique_path("temp_task_%%%%-%%%%-%%%%-%%%%");
        // Note that the provided tempname argument is a temporary file, call it "tempA".
        // Leatherman.curl during the download method will create another temporary file,
        // call it "tempB", to save the downloaded file's contents in chunks before
        // renaming it to "tempA." The rationale behind this solution is that:
        //    (1) After download, we still need to check "tempA" to ensure that its sha matches
        //    the provided sha. So the downloaded task is not quite a "valid" file after this
        //    method is called; it's still temporary.
        //
        //    (2) It somewhat simplifies error handling if multiple threads try to download
        //    the same file.
        auto download_result = downloadFileWithCurl(master_uris, connect_timeout, timeout, client, tempname, file.get<lth_jc::JsonContainer>("uri"));
        if (!std::get<0>(download_result)) {
            throw Module::ProcessingError(lth_loc::format(
                "Downloading the task file {1} failed after trying all the available master-uris. Most recent error message: {2}",
                file.get<std::string>("filename"),
                std::get<1>(download_result)));
        }

        if (sha256 != calculateSha256(tempname.string())) {
            fs::remove(tempname);
            throw Module::ProcessingError(lth_loc::format("The downloaded file {1} has a SHA that differs from the provided SHA", filename));
        }
        fs::rename(tempname, destination);
        return destination;
    }

    // Downloads the file at the specified url into the provided path.
    // The downloaded task file's permissions will be set to rwx for user and rx for
    // group for non-Windows OSes.
    //
    // The method returns a tuple (success, err_msg). success is true if the file was downloaded;
    // false otherwise. err_msg contains the most recent http_file_download_exception's error
    // message; it is initially empty.
    std::tuple<bool, std::string> downloadFileWithCurl(const std::vector<std::string>& master_uris,
                                                        uint32_t connect_timeout_s,
                                                        uint32_t timeout_s,
                                                        lth_curl::client& client,
                                                        const fs::path& file_path,
                                                        const lth_jc::JsonContainer& uri) {
        auto endpoint = createUrlEndpoint(uri);
        std::tuple<bool, std::string> result = std::make_tuple(false, "");
        for (auto& master_uri : master_uris) {
            auto url = master_uri + endpoint;
            lth_curl::request req(url);

            // Request timeouts expect milliseconds.
            req.connection_timeout(connect_timeout_s*1000);
            req.timeout(timeout_s*1000);

            try {
                lth_curl::response resp;
                client.download_file(req, file_path.string(), resp, NIX_TASK_FILE_PERMS);
                if (resp.status_code() >= 400) {
                    throw lth_curl::http_file_download_exception(
                        req,
                        file_path.string(),
                        lth_loc::format("{1} returned a response with HTTP status {2}. Response body: {3}", url, resp.status_code(), resp.body()));
                }
            } catch (lth_curl::http_file_download_exception& e) {
                // Server-side error, do nothing here -- we want to try the next master-uri.
                LOG_WARNING("Downloading the task file from the master-uri '{1}' failed. Reason: {2}", master_uri, e.what());
                std::get<1>(result) = e.what();
            } catch (lth_curl::http_request_exception& e) {
                // For http_curl_setup and http_file_operation exceptions
                throw Module::ProcessingError(lth_loc::format("Downloading the task file failed. Reason: {1}", e.what()));
            }

            if (fs::exists(file_path)) {
                std::get<0>(result) = true;
                return result;
            }
        }

        return result;
    }

    std::string createUrlEndpoint(const lth_jc::JsonContainer& uri) {
        std::string url = uri.get<std::string>("path");
        auto params = uri.getWithDefault<lth_jc::JsonContainer>("params", lth_jc::JsonContainer());
        if (params.empty()) {
            return url;
        }
        auto curl_handle = lth_curl::curl_handle();
        url += "?";
        for (auto& key : params.keys()) {
            auto escaped_key = std::string(lth_curl::curl_escaped_string(curl_handle, key));
            auto escaped_val = std::string(lth_curl::curl_escaped_string(curl_handle, params.get<std::string>(key)));
            url += escaped_key + "=" + escaped_val + "&";
        }
        // Remove trailing ampersand (&)
        url.pop_back();
        return url;
    }

    // Computes the sha256 of the file denoted by path. Assumes that
    // the file designated by "path" exists.
    std::string calculateSha256(const std::string& path) {
        auto mdctx = EVP_MD_CTX_create();

        EVP_DigestInit_ex(mdctx, EVP_sha256(), nullptr);
        {
            constexpr std::streamsize CHUNK_SIZE = 0x8000;  // 32 kB
            char buffer[CHUNK_SIZE];
            boost::nowide::ifstream ifs(path, std::ios::binary);

            while (ifs.read(buffer, CHUNK_SIZE)) {
                EVP_DigestUpdate(mdctx, buffer, CHUNK_SIZE);
            }
            if (!ifs.eof()) {
                EVP_MD_CTX_destroy(mdctx);
                throw Module::ProcessingError(lth_loc::format("Error while reading {1}", path));
            }
            EVP_DigestUpdate(mdctx, buffer, ifs.gcount());
        }

        unsigned char md_value[EVP_MAX_MD_SIZE];
        unsigned int md_len;

        EVP_DigestFinal_ex(mdctx, md_value, &md_len);
        EVP_MD_CTX_destroy(mdctx);

        std::string md_value_hex;

        md_value_hex.reserve(2*md_len);
        // TODO use boost::algorithm::hex_lower and drop the std::transform below when we upgrade to boost 1.62.0 or newer
        alg::hex(md_value, md_value+md_len, std::back_inserter(md_value_hex));
        std::transform(md_value_hex.begin(), md_value_hex.end(), md_value_hex.begin(), ::tolower);

        return md_value_hex;
    }

}  // namespace Util
}  // namespace PXPAgent