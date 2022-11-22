#include <pch.h>
#include "TrainingProgramListConfigurationControl.h"
#include "uuid_generator.h"
#include "file_dialogs.h"
#include <external/nlohmann/json.hpp>
#include <ostream>
#include <fstream>

using json = nlohmann::json;

// Allow serialization of std::chrono::milliseconds
namespace nlohmann {
	template <>
	struct adl_serializer<std::chrono::milliseconds> {
		static void to_json(json& j, const std::chrono::milliseconds& ms) { j = ms.count(); }
		static void from_json(const json& j, std::chrono::milliseconds& ms) { ms = std::chrono::milliseconds(j); }
	};
}

namespace configuration
{
	// Allow serialization of training programs
	NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(configuration::TrainingProgramEntry, Name, Duration, Type, TrainingPackCode, WorkshopMapPath);
	NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(configuration::TrainingProgramData, Id, Name, Duration, Entries, ReadOnly);
}


template <typename T>
bool removeOne(std::vector<T>& vec, const T& value)
{
    auto it = std::find(vec.begin(), vec.end(), value);
    if (it != vec.end()) {
        vec.erase(it);
        return true;
    }
    return false;
}

namespace configuration
{
    TrainingProgramListConfigurationControl::TrainingProgramListConfigurationControl(
        std::shared_ptr<std::map<std::string, TrainingProgramData>> trainingProgramData,
        std::shared_ptr<ITrainingProgramRepository> repository)
        : _trainingProgramData{ std::move(trainingProgramData) }
        , _repository{ repository }
    {

    }

    void TrainingProgramListConfigurationControl::registerTrainingProgramListReceiver(std::shared_ptr<ITrainingProgramListReceiver> receiver)
    {
        _receivers.push_back(receiver);
    }

    std::string TrainingProgramListConfigurationControl::addTrainingProgram()
    {
        auto data = TrainingProgramData{ };
        data.Id = uuid_generator::generateUUID();
        data.Name = "New Training Program";
        data.Duration = std::chrono::milliseconds(0);

        _trainingProgramData->emplace(data.Id, data);
        _trainingProgramOrder.push_back(data.Id);

        notifyReceivers();

        return data.Id;        
    }


    void TrainingProgramListConfigurationControl::removeTrainingProgram(std::string trainingProgramId)
    {
        ensureIdIsKnown(trainingProgramId, "training program ID");

        removeOne(_trainingProgramOrder, trainingProgramId);
        _trainingProgramData->erase(trainingProgramId);

        notifyReceivers();
    }

    void TrainingProgramListConfigurationControl::swapTrainingPrograms(std::string firstProgramId, std::string secondProgramId)
    {
        ensureIdIsKnown(firstProgramId, "first training program ID");
        ensureIdIsKnown(secondProgramId, "second training program ID");

        auto firstIter = std::find(_trainingProgramOrder.begin(), _trainingProgramOrder.end(), firstProgramId);
        auto secondIter = std::find(_trainingProgramOrder.begin(), _trainingProgramOrder.end(), secondProgramId);
        if (firstIter != _trainingProgramOrder.end() && secondIter != _trainingProgramOrder.end())
        {
            std::iter_swap(firstIter, secondIter);
            notifyReceivers();
        }
        else
        {
            // This should not be possible unless ensureIdIsKnown has an error
            throw std::runtime_error("Training Program List is inconsistent");
        }
    }

    void TrainingProgramListConfigurationControl::changeWorkshopFolderLocation(const std::string& newLocation)
    {
        _workshopFolderLocation = newLocation;
        notifyReceivers();
    }

    void TrainingProgramListConfigurationControl::injectTrainingProgram(const TrainingProgramData& data)
    {
        if (_trainingProgramData->count(data.Id) > 0)
        {
            LOG("Replacing existing training program with uuid {}", data.Id);
            _trainingProgramData->erase(data.Id);
            // We can keep the training program order, because we'll add the program right back.
        }
        else
        {
            LOG("Injecting new training program with uuid {}", data.Id);
            _trainingProgramOrder.insert(_trainingProgramOrder.begin(), data.Id);
        }
        _trainingProgramData->try_emplace(data.Id, data);
        notifyReceivers();

        LOG("Successfully injected/updated training program");
    }

    /** Provides a copy of the training program list data (e.g. for display). */
    TrainingProgramListData TrainingProgramListConfigurationControl::getTrainingProgramList() const
    {
        TrainingProgramListData data;
        data.TrainingProgramOrder = std::vector<std::string>(_trainingProgramOrder);
        data.WorkshopFolderLocation = _workshopFolderLocation;
        for (auto& [id, programData]: *_trainingProgramData)
        {
            data.TrainingProgramData.emplace(id, programData);
        }
        return data;
    }

    /** Provides a copy of data of a single training program (e.g. for display). */
    TrainingProgramData TrainingProgramListConfigurationControl::getTrainingProgramData(std::string trainingProgramId) const
    {
        ensureIdIsKnown(trainingProgramId, "training program ID");
        return (*_trainingProgramData)[trainingProgramId];
    }

    void TrainingProgramListConfigurationControl::restoreData(const std::filesystem::path &path)
    {
        _trainingProgramData->clear();
        _trainingProgramOrder.clear();

        // Read data from the repo
		TrainingProgramListData data;
		if(path.empty())
			data = _repository->restoreData();
		else
			data = _repository->restoreData(path);

        // Convert to internal data structure
        _trainingProgramOrder = data.TrainingProgramOrder;
        for (auto [id, programData] : data.TrainingProgramData)
        {
            _trainingProgramData->emplace(id, programData);
        }
        _workshopFolderLocation = data.WorkshopFolderLocation;

        // Notify receivers, but do not write the file (would be kinda pointless right here)
        notifyReceivers(true);
    }


	void TrainingProgramListConfigurationControl::loadTrainingPrograms()
    {
		LOG("Load Training programs..");
		auto path = file_dialogs::getOpenFilePath("", {"json"});
		restoreData(path);
	}

	void TrainingProgramListConfigurationControl::saveTrainingPrograms()
    {
		LOG("Save Training programs..");
		auto path = file_dialogs::getSaveFilePath("", {"json"});
		_repository->storeData(getTrainingProgramList(), path);
	}

	void TrainingProgramListConfigurationControl::loadTrainingProgram()
    {
		LOG("Load Training program..");
		// get a file path to load from
		auto path = file_dialogs::getOpenFilePath("", {"json"});
		if (!std::filesystem::exists(path)) { 
			LOG("File does not exist");
			return; 
		}

		// load the full contents of the file into a string
		std::string serialized;
		std::stringstream buffer;
		std::ifstream is{ path };
		buffer << is.rdbuf();
		is.close();
		serialized = buffer.str();

		// try to parse it as json
		json jsonData;
		try
		{
			jsonData = json::parse(serialized);
		}
		catch (json::exception ex)
		{
			LOG("Could not parse JSON Data: {}", ex.what());
			LOG("ERROR: Data in file was: {}", serialized);
			return;
		}

		// try to parse as training program
		TrainingProgramData trainingProgramData;
		try
		{
			trainingProgramData = jsonData.get<configuration::TrainingProgramData>();
		}
		catch (json::exception ex)
		{
			LOG("JSON Data does not match training program structure: {}", ex.what());
			LOG("ERROR: JSON Data was: {}", jsonData.dump(2));
			return;
		}

		// inject the loaded training program
		injectTrainingProgram(trainingProgramData);
	}

	void TrainingProgramListConfigurationControl::saveTrainingProgram(std::string trainingProgramId)
    {
		LOG("Save Training program {}..", trainingProgramId);
		// get a file path to save to
		auto path = file_dialogs::getSaveFilePath("", {"json"});
		
		// get the training program data
		auto trainingProgramData = getTrainingProgramData(trainingProgramId);
		// serialize it to json
		json jsonData = trainingProgramData;
		
		// write the json to the path
		std::ofstream os{ path };
		os << jsonData.dump(2);
		os.flush();
	}

    void TrainingProgramListConfigurationControl::notifyReceivers(bool currentlyRestoringData)
    {
        auto listData = getTrainingProgramList();
        for (const auto& receiver : _receivers)
        {
            receiver->receiveListData(listData);
        }

        if (!currentlyRestoringData)
        {
            _repository->storeData(listData);
        }
    }

    void TrainingProgramListConfigurationControl::ensureIdDoesntExist(std::string trainingProgramId) const
    {
        if (_trainingProgramData->count(trainingProgramId) > 0)
        {
            throw std::runtime_error(fmt::format("There is already a training program with ID {}", trainingProgramId));
        }
    }
    void TrainingProgramListConfigurationControl::ensureIdIsKnown(std::string trainingProgramId, const std::string& parameterName) const
    {
        if (_trainingProgramData->count(trainingProgramId) == 0)
        {
            throw std::runtime_error(fmt::format("There is no training program with ID {} (parameter {})", trainingProgramId, parameterName));
        }
    }
}