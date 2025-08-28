// src/ThumbnailDownloader.cpp
#include "ThumbnailDownloader.h"
#include <curl/curl.h>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <iomanip>

const std::string ThumbnailDownloader::GITHUB_THUMBNAILS_BASE = 
    "https://raw.githubusercontent.com/libretro-thumbnails/";

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
                                        const std::string& thumbnail_dir,
                                        ProgressCallback progress_callback) {
    if (m_is_downloading.load()) {
        std::cout << "[WARNING] Download already in progress" << std::endl;
        return;
    }
    
    // Créer le répertoire s'il n'existe pas
    try {
        std::filesystem::create_directories(thumbnail_dir);
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Failed to create thumbnail directory: " << e.what() << std::endl;
        return;
    }
    
    m_cancel_requested.store(false);
    
    // Lancer le thread de téléchargement
    if (m_download_thread.joinable()) {
        m_download_thread.join();
    }
    
    m_download_thread = std::thread(&ThumbnailDownloader::download_worker, this, 
                                   games, thumbnail_dir, progress_callback);
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
                                         const std::string thumbnail_dir,
                                         ProgressCallback progress_callback) {
    m_is_downloading.store(true);
    
    int total_games = games.size();
    int current_index = 0;
    int successful_downloads = 0;
    int skipped_existing = 0;
    
    std::cout << "[INFO] Starting thumbnail download for " << total_games << " games across all systems" << std::endl;
    
    for (const auto& game : games) {
        if (m_cancel_requested.load()) {
            std::cout << "[INFO] Download cancelled by user" << std::endl;
            break;
        }
        
        current_index++;
        double progress = static_cast<double>(current_index) / total_games * 100.0;
        
        // Mettre à jour le callback de progression
        if (progress_callback) {
            std::cout << "[DEBUG] Calling progress callback: " << current_index << "/" << total_games << " - " << progress << "%" << std::endl;
            progress_callback(game.description, current_index, total_games, progress);
        } else {
            std::cout << "[DEBUG] No progress callback available" << std::endl;
        }
        
        // Vérifier si le fichier existe déjà
        std::string cleaned_name = clean_filename_for_github(game.description);
        std::string filename = cleaned_name + ".png";
        std::string filepath = thumbnail_dir + "/" + filename;
        
        std::cout << "[DEBUG] Original: '" << game.description << "' -> Cleaned: '" << cleaned_name << "'" << std::endl;
        
        if (std::filesystem::exists(filepath)) {
            std::cout << "[INFO] Skipping existing thumbnail: " << filename << std::endl;
            skipped_existing++;
            continue;
        }
        
        // Télécharger le thumbnail
        if (download_single_thumbnail(cleaned_name, game.system, thumbnail_dir)) {
            successful_downloads++;
            std::cout << "[SUCCESS] Downloaded: " << filename << std::endl;
        } else {
            std::cout << "[WARNING] Failed to download: " << filename << std::endl;
        }
        
        // Petit délai pour ne pas surcharger le serveur
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    m_is_downloading.store(false);
    
    std::cout << "[INFO] Thumbnail download completed!" << std::endl;
    std::cout << "[INFO] Downloaded: " << successful_downloads 
              << ", Skipped: " << skipped_existing 
              << ", Total: " << total_games << std::endl;
    
    // Callback final
    if (progress_callback && !m_cancel_requested.load()) {
        progress_callback("Download completed!", total_games, total_games, 100.0);
    }
}

bool ThumbnailDownloader::download_single_thumbnail(const std::string& cleaned_filename,
                                                   const std::string& system,
                                                   const std::string& thumbnail_dir) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        return false;
    }
    
    // Déterminer le repository selon le système
    std::string repository = get_repository_for_system(system);
    
    // Construire l'URL complète
    std::string url = GITHUB_THUMBNAILS_BASE + repository + "/master/Named_Snaps/" + url_encode(cleaned_filename) + ".png";
    std::string filepath = thumbnail_dir + "/" + cleaned_filename + ".png";
    
    std::cout << "[DEBUG] System: " << system << " -> Repository: " << repository << std::endl;
    std::cout << "[DEBUG] Trying to download: " << url << std::endl;
    
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