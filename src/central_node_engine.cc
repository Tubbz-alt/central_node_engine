#include <central_node_engine.h>
#include <stdint.h>
#include <log_wrapper.h>

#ifdef LOG_ENABLED
using namespace easyloggingpp;
static Logger *engineLogger;
#endif 

Engine::Engine() :
  checkFaultTime(5, "Evaluation time") {
#ifdef LOG_ENABLED
  engineLogger = Loggers::getLogger("ENGINE");
#endif
}


int Engine::loadConfig(std::string yamlFileName) {
  MpsDb *db = new MpsDb();
  mpsDb = shared_ptr<MpsDb>(db);

  if (mpsDb->load(yamlFileName) != 0) {
    errorStream.str(std::string());
    errorStream << "ERROR: Failed to load yaml database ("
		<< yamlFileName << ")";
    throw(EngineException(errorStream.str()));
  }

  LOG_TRACE("ENGINE", "YAML Database loaded from " << yamlFileName);

  // When the first yaml configuration file is loaded the bypassMap
  // must be created.  
  if (!bypassManager) {
    bypassManager = BypassManagerPtr(new BypassManager());
    bypassManager->createBypassMap(mpsDb);
  }

  LOG_TRACE("ENGINE", "BypassManager created");

  try {
    mpsDb->configure();
  } catch (DbException e) {
    delete db;
    throw e;
  }

  LOG_TRACE("ENGINE", "MPS Database configured from YAML");

  // Assign bypass to each digital/analog input
  bypassManager->assignBypass(mpsDb);

  // Find the lowest/highest BeamClasses - used when checking faults
  uint32_t num = 0;
  uint32_t lowNum = 100;
  for (DbBeamClassMap::iterator beamClass = mpsDb->beamClasses->begin();
       beamClass != mpsDb->beamClasses->end(); ++beamClass) {
    if ((*beamClass).second->number > num) {
      highestBeamClass = (*beamClass).second;
      num = (*beamClass).second->number;
    }
    if ((*beamClass).second->number < lowNum) {
      lowestBeamClass = (*beamClass).second;
      lowNum = (*beamClass).second->number;
    }
  }

  LOG_TRACE("ENGINE", "Lowest beam class found: " << lowestBeamClass->number);
  LOG_TRACE("ENGINE", "Highest beam class found: " << highestBeamClass->number);

  return 0;
}

// Update values from firmware...
void Engine::updateInputs() {
}

/**
 * Prior to check digital/analog faults assign the highest beam class available
 * as tentativeBeamClass for all mitigation devices. Also temporarily sets the
 * allowedBeamClass as the lowest beam class available.
 *
 * As faults are detected the tentativeBeamClass gets lowered accordingly.
 */
void Engine::setTentativeBeamClass() {
  // Assigns highestBeamClass as tentativeBeamClass for all MitigationDevices
  for (DbMitigationDeviceMap::iterator device = mpsDb->mitigationDevices->begin();
       device != mpsDb->mitigationDevices->end(); ++device) {
    (*device).second->tentativeBeamClass = highestBeamClass;
    (*device).second->allowedBeamClass = lowestBeamClass;
    LOG_TRACE("ENGINE", (*device).second->name << " tentative class set to: "
	      << (*device).second->tentativeBeamClass->number
	      << "; allowed class set to: "
	      << (*device).second->allowedBeamClass->number);
  }
}

/**
 * After checking the faults move the final tentativeBeamClass into the 
 * allowedBeamClass for the mitigation devices.
 */
void Engine::setAllowedBeamClass() {
  for (DbMitigationDeviceMap::iterator it = mpsDb->mitigationDevices->begin(); 
       it != mpsDb->mitigationDevices->end(); ++it) {
    (*it).second->allowedBeamClass = (*it).second->tentativeBeamClass;
    LOG_TRACE("ENGINE", (*it).second->name << " allowed class set to "
	      << (*it).second->allowedBeamClass->number);
  }
}

/**
 * First goes through the list of DigitalDevices and updates the current state.
 * Second updates the DigitalFaultStates for each Fault.
 * At the end of this method all Digital Faults will have updated values,
 * i.e. the field 'faulted' gets updated based on the current machine state. 
 */
void Engine::evaluateFaults() {
  std::stringstream errorStream;

  bool faulted = false;

  // Update digital device values based on individual inputs
  for (DbDigitalDeviceMap::iterator device = mpsDb->digitalDevices->begin(); 
       device != mpsDb->digitalDevices->end(); ++device) {
    uint32_t deviceValue = 0;
    LOG_TRACE("ENGINE", "Getting inputs for " << (*device).second->name << " device"
	      << ", there are " << (*device).second->inputDevices->size()
	      << " inputs");
    for (DbDeviceInputMap::iterator input = (*device).second->inputDevices->begin(); 
	 input != (*device).second->inputDevices->end(); ++input) {
      uint32_t inputValue = 0;
      // Check if the input has a valid bypass value
      if ((*input).second->bypass->status == BYPASS_VALID) {
	inputValue = (*input).second->bypass->value;
	LOG_TRACE("ENGINE", (*device).second->name << " bypassing input value to "
		  << (*input).second->bypass->value << " (actual value is "
		  << (*input).second->value << ")");
      }
      else {
	inputValue = (*input).second->value;
      }
      inputValue <<= (*input).second->bitPosition;
      deviceValue |= inputValue;
    }
    (*device).second->update(deviceValue);
    LOG_TRACE("ENGINE", (*device).second->name << " current value " << std::hex << deviceValue << std::dec);
  }  

  // At this point all digital devices have an updated value

  // Update digital & analog Fault values and MitigationDevice allowed class
  for (DbFaultMap::iterator fault = mpsDb->faults->begin();
       fault != mpsDb->faults->end(); ++fault) {

    // First calculate the digital Fault value from its one or more digital device inputs
    uint32_t faultValue = 0;
    for (DbFaultInputMap::iterator input = (*fault).second->faultInputs->begin();
	 input != (*fault).second->faultInputs->end(); ++input) {
      uint32_t inputValue = 0;
      if ((*input).second->digitalDevice) {
	inputValue = (*input).second->digitalDevice->value;
      }
      else {
	inputValue = (*input).second->analogDevice->value;
      }
      faultValue |= (inputValue << (*input).second->bitPosition);
      LOG_TRACE("ENGINE", (*fault).second->name << " current value " << std::hex << faultValue
		<< ", input value " << inputValue << std::dec << " bit pos "
		<< (*input).second->bitPosition);
    }
    (*fault).second->update(faultValue);
    LOG_TRACE("ENGINE", (*fault).second->name << " current value " << std::hex << faultValue << std::dec);

    // Now that a Fault has a new value check if it is in the FaultStates list,
    // and update the allowedBeamClass for the MitigationDevices
    for (DbDigitalFaultStateMap::iterator state = (*fault).second->digitalFaultStates->begin();
	 state != (*fault).second->digitalFaultStates->end(); ++state) {
      uint32_t maskedValue = faultValue & (*state).second->deviceState->mask;
      if ((*state).second->deviceState->value == maskedValue) {
	(*state).second->faulted = true;
	faulted = true; // Signal that at least one state is faulted
	LOG_TRACE("ENGINE", (*fault).second->name << " is faulted value=" 
		  << faultValue << ", masked=" << maskedValue
		  << " (fault state="
		  << (*state).second->deviceState->name
		  << ", value=" << (*state).second->deviceState->value << ")");
      }
      else {
	(*state).second->faulted = false;
      }
    }

    // If there are no faults, then enable the default - if there is one
    if (!faulted && (*fault).second->defaultDigitalFaultState) {
      (*fault).second->defaultDigitalFaultState->faulted = true;
      LOG_TRACE("ENGINE", (*fault).second->name << " is faulted value=" 
		<< faultValue << " (Default) fault state="
		<< (*fault).second->defaultDigitalFaultState->deviceState->name);
    }
  }
}

void Engine::evaluateIgnoreConditions() {
  // Calculate state of conditions
  for (DbConditionMap::iterator condition = mpsDb->conditions->begin();
       condition != mpsDb->conditions->end(); ++condition) {
    uint32_t conditionValue = 0;
    for (DbConditionInputMap::iterator input = (*condition).second->conditionInputs->begin();
	 input != (*condition).second->conditionInputs->end(); ++input) {
      uint32_t inputValue = 1;
      if ((*input).second->digitalFaultState) {
 	if (!(*input).second->digitalFaultState->faulted) {
	  inputValue = 0;
	}
      }
      else if ((*input).second->analogFaultState) {
 	if (!(*input).second->analogFaultState->faulted) {
	  inputValue = 0;
	}
      }
      conditionValue |= (inputValue << (*input).second->bitPosition);
      LOG_TRACE("ENGINE", "Condition " << (*condition).second->name << " current value " << std::hex << conditionValue
		<< ", input value " << inputValue << std::dec << " bit pos "
		<< (*input).second->bitPosition);
    }
    if ((*condition).second->mask == conditionValue) {
      (*condition).second->state = true;
    }
    else {
      (*condition).second->state = false;
    }
    LOG_TRACE("ENGINE",  "Condition " << (*condition).second->name << " is " << (*condition).second->state);

    for (DbIgnoreConditionMap::iterator ignoreCondition = (*condition).second->ignoreConditions->begin();
	 ignoreCondition != (*condition).second->ignoreConditions->end(); ++ignoreCondition) {
      // If ignore condition is true, update the fault state (digital or analog)
      if ((*ignoreCondition).second->digitalFaultState) {
	(*ignoreCondition).second->digitalFaultState->ignored = (*condition).second->state;
      }
      else if ((*ignoreCondition).second->analogFaultState) {
	(*ignoreCondition).second->analogFaultState->ignored = (*condition).second->state;
      }
    }
  }
}

void Engine::mitigate() {
  // Update digital Fault values and MitigationDevice allowed class
  for (DbFaultMap::iterator fault = mpsDb->faults->begin();
       fault != mpsDb->faults->end(); ++fault) {

    for (DbDigitalFaultStateMap::iterator state = (*fault).second->digitalFaultStates->begin();
	 state != (*fault).second->digitalFaultStates->end(); ++state) {
      if ((*state).second->faulted && !(*state).second->ignored) {
	LOG_TRACE("ENGINE", (*fault).second->name << " is faulted value=" 
		  << (*fault).second->value
		  << " (fault state="
		  << (*state).second->deviceState->name
		  << ", value=" << (*state).second->deviceState->value << ")");
	if ((*state).second->allowedClasses) {
	  for (DbAllowedClassMap::iterator allowed = (*state).second->allowedClasses->begin();
	       allowed != (*state).second->allowedClasses->end(); ++allowed) {
	    if ((*allowed).second->mitigationDevice->tentativeBeamClass->number >
		(*allowed).second->beamClass->number) {
	      (*allowed).second->mitigationDevice->tentativeBeamClass =
		(*allowed).second->beamClass;
	      LOG_TRACE("ENGINE", (*allowed).second->mitigationDevice->name << " tentative class set to "
			<< (*allowed).second->beamClass->number);
	    }
	  }
	}
      }
    }

    // If there are no faults, then enable the default - if there is one
    if ((*fault).second->defaultDigitalFaultState) {
      if ((*fault).second->defaultDigitalFaultState->faulted &&
	  !(*fault).second->defaultDigitalFaultState->ignored) {
	LOG_TRACE("ENGINE", (*fault).second->name << " is faulted value=" 
		  << (*fault).second->value << " (Default) fault state="
		  << (*fault).second->defaultDigitalFaultState->deviceState->name);
	if ((*fault).second->defaultDigitalFaultState->allowedClasses) {
	  for (DbAllowedClassMap::iterator allowed = (*fault).second->defaultDigitalFaultState->allowedClasses->begin();
	       allowed != (*fault).second->defaultDigitalFaultState->allowedClasses->end(); ++allowed) {
	    if ((*allowed).second->mitigationDevice->tentativeBeamClass->number >
		(*allowed).second->beamClass->number) {
	      (*allowed).second->mitigationDevice->tentativeBeamClass =
		(*allowed).second->beamClass;
	      LOG_TRACE("ENGINE", (*allowed).second->mitigationDevice->name << " tentative class set to "
			<< (*allowed).second->beamClass->number);
	    }
	  }
	}
      }
    }
  }
}

int Engine::checkFaults() {
  checkFaultTime.start();

  LOG_TRACE("ENGINE", "Checking faults");
  setTentativeBeamClass();
  evaluateFaults();
  evaluateIgnoreConditions();
  mitigate();
  setAllowedBeamClass();

  checkFaultTime.end();

  return 0;
}

void Engine::showFaults() {
  // Print current faults
  bool faults = false;

  // Digital Faults
  for (DbFaultMap::iterator fault = mpsDb->faults->begin();
       fault != mpsDb->faults->end(); ++fault) {
    for (DbDigitalFaultStateMap::iterator state = (*fault).second->digitalFaultStates->begin();
	 state != (*fault).second->digitalFaultStates->end(); ++state) {
      if ((*state).second->faulted) {
	if (!faults) {
	  std::cout << "# Current faults:" << std::endl;
	  faults = true;
	}
	std::cout << "  " << (*fault).second->name << ": " << (*state).second->deviceState->name
		  << " (value=" << (*state).second->deviceState->value << ", ignored="
		  << (*state).second->ignored << ")" << std::endl;
      }
    }
  }
  if (!faults) {
    std::cout << "# No faults" << std::endl;
  }
}

void Engine::showStats() {
  std::cout << ">> Engine Stats:" << std::endl;
  checkFaultTime.show();
}

void Engine::showMitigationDevices() {
  std::cout << ">> Mitigation Devices: " << std::endl;
  for (DbMitigationDeviceMap::iterator mitigation = mpsDb->mitigationDevices->begin();
       mitigation != mpsDb->mitigationDevices->end(); ++mitigation) {
    std::cout << (*mitigation).second->name << ":\t Allowed "
	      << (*mitigation).second->allowedBeamClass->number << "/ Tentative "
	      << (*mitigation).second->tentativeBeamClass->number << std::endl;
  }
}

void Engine::showDeviceInputs() {
  std::cout << "Device Inputs: " << std::endl;
  for (DbDeviceInputMap::iterator input = mpsDb->deviceInputs->begin();
       input != mpsDb->deviceInputs->end(); ++input) {
    std::cout << (*input).second << std::endl;
  }
}
