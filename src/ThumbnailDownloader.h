// src/ThumbnailDownloader.h
#pragma once

#include <string>
#include <vector>
#include <functional>
#include <thread>
#include <atomic>
#include "Game.h"

class ThumbnailDownloader {
public:
    // Callback: (current_file, current_index, total_count, progress_percentage)
    using ProgressCallback = std::function<void(const std::string&, int, int, double)>;
    
    ThumbnailDownloader();
    ~ThumbnailDownloader();
    
    // Démarre le téléchargement en arrière-plan
    void start_download(const std::vector<Game>& games, 
                       const std::string& thumbnail_dir,
                       ProgressCallback progress_callback = nullptr);
    
    // Arrête le téléchargement
    void cancel_download();
    
    // Vérifie si un téléchargement est en cours
    bool is_downloading() const;
    
private:
    std::thread m_download_thread;
    std::atomic<bool> m_is_downloading{false};
    std::atomic<bool> m_cancel_requested{false};
    
    // Télécharge un seul thumbnail
    bool download_single_thumbnail(const std::string& cleaned_filename,
                                  const std::string& system,
                                  const std::string& thumbnail_dir);
    
    // URL encode pour les noms de fichiers
    std::string url_encode(const std::string& text);
    
    // Nettoie le nom de fichier pour GitHub
    std::string clean_filename_for_github(const std::string& description);
    
    // Détermine le repository GitHub selon le système du jeu
    std::string get_repository_for_system(const std::string& system);
    
    // Download worker thread
    void download_worker(const std::vector<Game> games, 
                        const std::string thumbnail_dir,
                        ProgressCallback progress_callback);
    
    // Base URL template pour les thumbnails
    static const std::string GITHUB_THUMBNAILS_BASE;
};