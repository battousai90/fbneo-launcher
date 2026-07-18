// src/ThumbnailDownloader.cpp
#include "ThumbnailDownloader.h"
#include <curl/curl.h>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <iomanip>

const std::string ThumbnailDownloader::GITHUB_THUMBNAILS_BASE = 
    "https://raw.githubusercontent.com/finalburnneo/FBNeo-extras/main/";

ThumbnailDownloader::ThumbnailDownloader() {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

ThumbnailDownloader::~ThumbnailDownloader() {
    cancel_download();
    if (m_download_thread.joinable()) {
        m_download_thread.join();
    }
    curl_global_cleanup();
}

// Structure pour passer les données à curl
struct DownloadData {
    std::ofstream* file;
    size_t total_size;
    size_t downloaded_size;
};

// Callback pour écrire les données reçues - écrit immédiatement sur disque
static size_t write_callback(void* contents, size_t size, size_t nmemb, DownloadData* data) {
    size_t total_size = size * nmemb;
    data->file->write(static_cast<char*>(contents), total_size);
    data->file->flush();  // Force l'écriture immédiate sur disque
    data->downloaded_size += total_size;
    return total_size;
}

void ThumbnailDownloader::start_download(const std::vector<Game>& games,
                                        const std::string& artwork_dir,
                                        ArtworkType artwork_type,
                                        ProgressCallback progress_callback) {
    if (m_is_downloading.load()) {
        std::cout << "[WARNING] Download already in progress" << std::endl;
        return;
    }
    
    // Créer le répertoire s'il n'existe pas
    try {
        std::filesystem::create_directories(artwork_dir);
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Failed to create artwork directory: " << e.what() << std::endl;
        return;
    }
    
    m_cancel_requested.store(false);
    
    // Lancer le thread de téléchargement
    if (m_download_thread.joinable()) {
        m_download_thread.join();
    }
    
    m_download_thread = std::thread(&ThumbnailDownloader::download_worker, this, 
                                   games, artwork_dir, artwork_type, progress_callback);
}

void ThumbnailDownloader::cancel_download() {
    m_cancel_requested.store(true);
    if (m_download_thread.joinable()) {
        m_download_thread.join();
    }
}

bool ThumbnailDownloader::is_downloading() const {
    return m_is_downloading.load();
}

void ThumbnailDownloader::download_worker(const std::vector<Game> games,
                                         const std::string artwork_dir,
                                         ArtworkType artwork_type,
                                         ProgressCallback progress_callback) {
    m_is_downloading.store(true);
    
    int total_games = games.size();
    int current_index = 0;
    int successful_downloads = 0;
    int skipped_existing = 0;
    
    const char* artwork_type_str = (artwork_type == ArtworkType::Previews) ? "previews" : "titles";
    std::cout << "[INFO] Starting " << artwork_type_str << " download for " << total_games << " games" << std::endl;
    
    for (const auto& game : games) {
        if (m_cancel_requested.load()) {
            std::cout << "[INFO] Download cancelled by user" << std::endl;
            break;
        }
        
        current_index++;
        double progress = static_cast<double>(current_index) / total_games * 100.0;
        
        // Mettre à jour le callback de progression
        if (progress_callback) {
            // Show progress every 100 games to reduce console spam
            if (current_index % 100 == 0 || current_index == total_games) {
                std::cout << "[INFO] Progress: " << current_index << "/" << total_games << " (" << std::fixed << std::setprecision(1) << progress << "%)" << std::endl;
            }
            progress_callback(game.name, current_index, total_games, progress);
        }
        
        // Vérifier si le fichier existe déjà - use ROM name with system prefix
        std::string system_prefix = get_system_prefix(game.system);
        std::string filename = system_prefix + game.name + ".png";
        std::string filepath = artwork_dir + "/" + filename;
        
        if (std::filesystem::exists(filepath)) {
            skipped_existing++;
            continue;
        }
        
        // Télécharger le artwork - use ROM name with system prefix
        if (download_single_file(game.name, game.system, artwork_dir, artwork_type)) {
            successful_downloads++;
            std::cout << "[SUCCESS] Downloaded: " << filename << std::endl;
        } else {
            // Don't spam console with failed downloads - they're expected for many ROMs
            // std::cout << "[WARNING] Failed to download: " << filename << std::endl;
        }
        
        // Petit délai pour ne pas surcharger le serveur
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    m_is_downloading.store(false);
    
    int failed_downloads = total_games - successful_downloads - skipped_existing;
    double success_rate = (total_games > 0) ? (double(successful_downloads) / total_games * 100.0) : 0.0;
    
    std::cout << "[INFO] " << artwork_type_str << " download completed!" << std::endl;
    std::cout << "[INFO] Results: " << successful_downloads << " downloaded"
              << ", " << skipped_existing << " skipped (already exist)"
              << ", " << failed_downloads << " not available"
              << " (Success rate: " << std::fixed << std::setprecision(1) << success_rate << "%)" << std::endl;
    
    // Callback final
    if (progress_callback && !m_cancel_requested.load()) {
        progress_callback("Download completed!", total_games, total_games, 100.0);
    }
}

bool ThumbnailDownloader::download_single_file(const std::string& rom_name,
                                              const std::string& system,
                                              const std::string& artwork_dir,
                                              ArtworkType artwork_type) {
    // Déterminer le dossier selon le type d'artwork
    const char* folder = (artwork_type == ArtworkType::Previews) ? "previews" : "titles";

    // Obtenir le préfixe système
    std::string system_prefix = get_system_prefix(system);

    // Construire le chemin de destination pour FBNeo-extras avec préfixe système
    std::string filename_with_prefix = system_prefix + rom_name;
    std::string filepath = artwork_dir + "/" + filename_with_prefix + ".png";

    // Ne rien retélécharger si l'artwork est déjà présent (fichier non vide).
    // C'est le garde-fou commun aux deux chemins : téléchargement en masse ET
    // bouton « Download Art » pour un seul jeu.
    {
        std::error_code ec;
        if (std::filesystem::exists(filepath, ec) &&
            std::filesystem::file_size(filepath, ec) > 0 && !ec) {
            return true;
        }
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        return false;
    }

    // Construire l'URL complète pour FBNeo-extras avec préfixe système
    std::string url = GITHUB_THUMBNAILS_BASE + folder + "/" + url_encode(filename_with_prefix) + ".png";

    // Only show debug info for successful downloads
    // std::cout << "[DEBUG] Type: " << folder << std::endl;
    // std::cout << "[DEBUG] Trying to download: " << url << std::endl;
    
    // Ouvrir le fichier de destination
    std::ofstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        curl_easy_cleanup(curl);
        return false;
    }
    
    DownloadData data;
    data.file = &file;
    data.total_size = 0;
    data.downloaded_size = 0;
    
    // Configuration de CURL
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &data);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);  // 30 secondes timeout
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "FBNeo-Launcher/1.0");
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);  // Fail sur HTTP errors
    
    // Effectuer le téléchargement
    CURLcode res = curl_easy_perform(curl);
    
    long response_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
    
    curl_easy_cleanup(curl);
    file.close();
    
    if (res != CURLE_OK || response_code != 200) {
        // Supprimer le fichier partiel en cas d'erreur
        std::filesystem::remove(filepath);
        return false;
    }
    
    return true;
}

void ThumbnailDownloader::download_single_artwork(const std::string& game_name,
                                                 const std::string& game_system,
                                                 const std::string& artwork_dir,
                                                 ArtworkType artwork_type,
                                                 ProgressCallback progress_callback) {
    if (m_is_downloading.load()) {
        std::cout << "[WARNING] Download already in progress" << std::endl;
        return;
    }
    
    // Créer le répertoire s'il n'existe pas
    try {
        std::filesystem::create_directories(artwork_dir);
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Failed to create artwork directory: " << e.what() << std::endl;
        return;
    }
    
    m_cancel_requested.store(false);
    m_is_downloading.store(true);
    
    // Use ROM name directly for FBNeo-extras
    const char* artwork_type_str = (artwork_type == ArtworkType::Previews) ? "preview" : "title";
    
    if (progress_callback) {
        progress_callback("Downloading " + std::string(artwork_type_str) + " for " + game_name, 1, 1, 0.0);
    }
    
    // Download the single artwork using ROM name with system info
    bool success = download_single_file(game_name, game_system, artwork_dir, artwork_type);
    
    if (success) {
        std::cout << "[SUCCESS] Downloaded " << artwork_type_str << " for: " << game_name << std::endl;
    } else {
        std::cout << "[WARNING] Failed to download " << artwork_type_str << " for: " << game_name << std::endl;
    }
    
    m_is_downloading.store(false);
    
    // Final callback
    if (progress_callback && !m_cancel_requested.load()) {
        std::string status = success ? "Download completed!" : "Download failed!";
        progress_callback(status, 1, 1, 100.0);
    }
}

std::string ThumbnailDownloader::url_encode(const std::string& text) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        return text;
    }
    
    char* encoded = curl_easy_escape(curl, text.c_str(), text.length());
    if (!encoded) {
        curl_easy_cleanup(curl);
        return text;
    }
    
    std::string result(encoded);
    curl_free(encoded);
    curl_easy_cleanup(curl);
    
    return result;
}

std::string ThumbnailDownloader::clean_filename_for_github(const std::string& description) {
    std::string cleaned = description;
    
    // Supprimer les suffixes communs qui ne sont pas dans les noms GitHub
    size_t pos;
    
    // Supprimer ": The ..." et remplacer par juste le nom principal
    if ((pos = cleaned.find(": The ")) != std::string::npos) {
        cleaned = cleaned.substr(0, pos);
    }
    if ((pos = cleaned.find(": ")) != std::string::npos) {
        cleaned = cleaned.substr(0, pos);
    }
    
    // Supprimer les parenthèses et leur contenu (sauf pour les apostrophes au début)
    if (cleaned[0] != '\'') {  // Garder les noms comme "'88 Games"
        if ((pos = cleaned.find(" (")) != std::string::npos) {
            cleaned = cleaned.substr(0, pos);
        }
    }
    
    // Nettoyer les caractères problématiques mais garder les apostrophes importantes
    // Les apostrophes au début sont importantes pour GitHub ('88 Games, '96 Flag Rally)
    
    return cleaned;
}

std::string ThumbnailDownloader::get_system_prefix(const std::string& system) {
    // Mapping des systèmes vers les préfixes de fichier pour FBNeo-extras
    if (system.find("Fairchild_Channel_F") != std::string::npos) {
        return "chf_";
    } else if (system.find("ColecoVision") != std::string::npos) {
        return "cv_";
    } else if (system.find("Sega_Game_Gear") != std::string::npos) {
        return "gg_";
    } else if (system.find("MegaDrive") != std::string::npos) {
        return "md_";
    } else if (system.find("TurboGrafx-16") != std::string::npos) {
        return "tg_";
    } else if (system.find("MSX") != std::string::npos) {
        return "msx_";
    } else if (system.find("Sega_Master_System") != std::string::npos) {
        return "sms_";
    } else if (system.find("Nintendo_Entertainment_System") != std::string::npos) {
        return "nes_";
    } else if (system.find("Neo_Geo_Pocket") != std::string::npos) {
        return "ngp_";
    } else if (system.find("PC_ENGINE") != std::string::npos) {
        return "pce_";
    } else if (system.find("Nintendo_Famicom_Disk_System") != std::string::npos) {
        return "fds_";
    } else if (system.find("Super_Nintendo_Entertainment_System") != std::string::npos) {
        return "snes_";
    } else if (system.find("Sinclair_ZX_Spectrum") != std::string::npos) {
        return "spec_";
    } else if (system.find("Sega_SG-1000") != std::string::npos) {
        return "sg1k_";
    } else if (system.find("PC_Engine_SuperGrafx") != std::string::npos) {
        return "sgx_";
    } else {
        // Arcade et Neo Geo n'ont pas de préfixe
        return "";
    }
}

std::string ThumbnailDownloader::get_repository_for_system(const std::string& system) {
    // Mapping DAT système vers repository GitHub
    if (system.find("Arcade") != std::string::npos) {
        return "FBNeo_-_Arcade_Games";
    } else if (system.find("Neogeo") != std::string::npos) {
        return "SNK_-_Neo_Geo";
    } else if (system.find("PC_Engine_SuperGrafx") != std::string::npos) {
        return "NEC_-_PC_Engine_SuperGrafx";
    } else if (system.find("Super_Nintendo_Entertainment_System") != std::string::npos) {
        return "Nintendo_-_Super_Nintendo_Entertainment_System";
    } else if (system.find("ColecoVision") != std::string::npos) {
        return "Coleco_-_ColecoVision";
    } else if (system.find("Neo_Geo_Pocket") != std::string::npos) {
        return "SNK_-_Neo_Geo_Pocket";
    } else if (system.find("Sega_Game_Gear") != std::string::npos) {
        return "Sega_-_Game_Gear";
    } else if (system.find("TurboGrafx-16") != std::string::npos) {
        return "NEC_-_PC_Engine_-_TurboGrafx_16";
    } else if (system.find("Fairchild_Channel_F") != std::string::npos) {
        return "Fairchild_-_Channel_F";
    } else if (system.find("Nintendo_Entertainment_System") != std::string::npos) {
        return "Nintendo_-_Nintendo_Entertainment_System";
    } else if (system.find("Sega_Master_System") != std::string::npos) {
        return "Sega_-_Master_System_-_Mark_III";
    } else if (system.find("MegaDrive") != std::string::npos) {
        return "Sega_-_Mega_Drive_-_Genesis";
    } else if (system.find("Nintendo_Famicom_Disk_System") != std::string::npos) {
        return "Nintendo_-_Family_Computer_Disk_System";
    } else if (system.find("Sega_SG-1000") != std::string::npos) {
        return "Sega_-_SG-1000";
    } else if (system.find("MSX") != std::string::npos) {
        return "Microsoft_-_MSX";
    } else if (system.find("PC_ENGINE") != std::string::npos) {
        return "NEC_-_PC_Engine_-_TurboGrafx_16"; // Fallback pour PC ENGINE
    } else if (system.find("Sinclair_ZX_Spectrum") != std::string::npos) {
        return "Sinclair_-_ZX_Spectrum";
    } else {
        // Fallback : arcade par défaut
        return "FBNeo_-_Arcade_Games";
    }
}