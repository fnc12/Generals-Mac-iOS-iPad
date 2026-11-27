#pragma once

#include "game_files.h"
#include "ini_objects.h"
#include "map_document.h"
#include "map_editor.h"
#include "map_file.h"
#include "model_loader.h"
#include "storage.h"
#include "terrain_ini.h"

#include <chrono>
#include <deque>
#include <optional>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace end_editor {

    // The editor logic behind the C ABI (editor_core_c.h). Not thread safe: the client calls it
    // from one thread.
    class EditorCore {
      public:
        // Opens (creates) the store and sets status to "idle". Throws std::system_error on failure.
        explicit EditorCore(std::string dbPath);

        const std::string& dbPath() const {
            return this->path;
        }

        // The map name of the test copy map.play writes into the user Maps folder (the user's own
        // map file is not touched by Play).
        static constexpr const char* testMapName = "__EditorTest";

        // Returns "" when the command is accepted, "error:<code>" otherwise.
        std::string send(std::string_view command);

        // Runs queued work.
        void update();

        bool hasPendingWork() const {
            return !this->jobs.empty();
        }
        std::size_t pendingJobCount() const {
            return this->jobs.size();
        }

      private:
        struct Job {
            enum Kind {
                openGame,
                openModel,
                playAnimation,
                openMap,
                loadMapModels,
                editMap,
                saveMap,
                listMaps,
                playMap
            } kind = openGame;
            std::vector<std::string> arguments;
        };

        void setStatus(const std::string& state,
                       const std::string& message,
                       const std::string& gameDir,
                       std::int64_t objectCount);
        void setModelState(const std::string& name, const std::string& state, const std::string& message);
        void runOpenGame(const Job& job);
        void runOpenModel(const std::vector<std::string>& arguments);
        void runPlayAnimation(const std::string& name);
        void setMapState(const std::string& path, const std::string& state, const std::string& message);
        void clearMap();
        void listMaps();
        void runOpenMap(const std::string& path);
        void runLoadMapModels();
        void queueBeforeModelLoading(Job job);
        void runEditMap(const std::vector<std::string>& arguments);
        // map.save (any file) and map.save_as (a name in the user Maps folder; arguments: path,
        // "as", optional "replace").
        void runSaveMap(const std::vector<std::string>& arguments);
        // Writes the open document and its preview to `target`; "" when written, else what failed
        // (a failed preview sets only `previewError`).
        std::string writeMapFiles(const std::filesystem::path& target, std::string& previewError);
        // map.play: the open map with its edits to the test copy, then the game started on it.
        void runPlayMap();
        // play.*: a setting of the Play button; "" or an error code (the note says why).
        std::string setPlaySetting(std::string_view name, const std::vector<std::string>& arguments);
        // The default game app for the opened game (GeneralsXZH or GeneralsX in ~/GeneralsX/Apps).
        std::string defaultGameApp() const;
        void writePlay();
        // The started game ended: the play row says so.
        void pollPlay();
        void relistMaps();
        // Writes one object of the open document to map_objects / map_object_properties (and
        // queues its model when the map has not used it yet); removes the rows when it is gone.
        void writeMapObject(std::int64_t id);
        void updateMapEditState(const std::string& message);
        // map_links from the document's waypoint links and road segments.
        void writeMapLinks();
        // The map_editor row and map_selection, when they changed.
        void publishEditor();
        // A refused command: the note right away (send() returns the error code too).
        std::string refuse(const std::string& error, const std::string& note);
        std::string checkEditCommand(std::string_view name, const std::vector<std::string>& arguments);
        int roadKind(const std::string& name) const;
        const ObjectInfo* findObject(const std::string& templateName) const;

        std::string path;
        std::unique_ptr<Storage> storage;
        std::deque<Job> jobs;
        std::int64_t revision = 0;
        GameFiles files;
        std::unique_ptr<ModelLoader> loader;
        LoadedModel model;

        // Template names compared without case (ASCII); looked up by a view, without a copy.
        struct NoCaseLess {
            using is_transparent = void;
            bool operator()(std::string_view a, std::string_view b) const;
        };
        std::vector<ObjectInfo> objectInfos;
        // Template name (a view of objectInfos' name) -> objectInfos.
        std::map<std::string_view, std::size_t, NoCaseLess> objectsByName;
        // Models of the open map still to load, in map_models id order.
        struct PendingMapModel {
            std::int64_t id = 0;
            std::string request;  // model \t muzzle flashes \t hidden sub-objects
        };
        std::vector<PendingMapModel> mapModels;
        std::size_t nextMapModel = 0;
        std::set<std::string> mapTextures;  // names already in map_model_textures
        std::map<std::string, std::int64_t> mapModelIds;  // model request -> map_models id
        bool mapModelJobQueued = false;
        std::unique_ptr<MapDocument> document;  // the open map, for editing and saving
        std::vector<std::array<float, 3>> tileColors;  // of the open map, for its preview
        std::vector<RoadType> roadTypes;  // of the opened game
        MapEditor editor;  // the user's tool, selection, clipboard and note; outlives the maps
        MapEditor::State published;
        std::int64_t publishedSelection = -1;
        std::string userMapsFolder;  // the game's user Maps folder, listed with the game's maps
        std::int64_t mapListRevision = 0;
        std::vector<std::filesystem::path> gameFolders;  // map.save never writes inside them
        std::vector<std::string> factionNames;  // PlayerTemplate.ini's playable factions
        PlayRow play;  // the Play button's settings and its last run (the store's play row)
        // play.stop asked the game to quit at this time; it is ended for good 3 s later.
        std::optional<std::chrono::steady_clock::time_point> playStopAsked;
    };
}
