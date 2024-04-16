//
// Copyright (C) 2006-2017 Christoph Sommer <sommer@ccs-labs.org>
//
// Documentation for these modules is at http://veins.car2x.org/
//
// SPDX-License-Identifier: GPL-2.0-or-later
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
//

#include <fstream>
#include <vector>
#include <algorithm>
#include <stdexcept>
#include <iterator>

#include "veins/modules/mobility/traci/TraCIScenarioManager.h"
#include "veins/modules/mobility/traci/TraCICommandInterface.h"
#include "veins/modules/mobility/traci/TraCIConstants.h"
#include "veins/modules/mobility/traci/TraCIMobility.h"
#include "veins/modules/obstacle/ObstacleControl.h"
#include "veins/modules/floor/FloorControl.h"
#include "veins/modules/tunnel/TunnelControl.h"
#include "veins/modules/world/traci/trafficLight/TraCITrafficLightInterface.h"

#define LENGTH 4.5 // assumed vehicle length

using namespace Veins::TraCIConstants;

using Veins::AnnotationManagerAccess;
using Veins::TraCIBuffer;
using Veins::TraCICoord;
using Veins::TraCIScenarioManager;
using Veins::TraCITrafficLightInterface;

Define_Module(Veins::TraCIScenarioManager);

const std::string TraCIScenarioManager::TRACI_INITIALIZED_SIGNAL_NAME = "traciInitialized";

TraCIScenarioManager::TraCIScenarioManager() :
        mobRng(0), connection(0), connectAndStartTrigger(0), executeOneTimestepTrigger(0), world(0), cc(0), traciInitializedSignal(
                registerSignal(TRACI_INITIALIZED_SIGNAL_NAME.c_str()))
{
}

TraCIScenarioManager::~TraCIScenarioManager()
{
    cancelAndDelete(connectAndStartTrigger);
    cancelAndDelete(executeOneTimestepTrigger);
    delete commandIfc;
    delete connection;
    delete[] hostsGrid;
}

std::vector<std::string> getMapping(std::string el)
{

    // search for string protection characters '
    char protection = '\'';
    size_t first = el.find(protection);
    size_t second;
    size_t eq;
    std::string type, value;
    std::vector<std::string> mapping;

    if (first == std::string::npos) {
        // there's no string protection, simply split by '='
        cStringTokenizer stk(el.c_str(), "=");
        mapping = stk.asVector();
    }
    else {
        // if there's string protection, we need to find a matching delimiter
        second = el.find(protection, first + 1);
        // ensure that a matching delimiter exists, and that it is at the end
        if (second == std::string::npos || second != el.size() - 1) throw cRuntimeError("invalid syntax for mapping \"%s\"", el.c_str());

        // take the value of the mapping as the text within the quotes
        value = el.substr(first + 1, second - first - 1);

        if (first == 0) {
            // if the string starts with a quote, there's only the value
            mapping.push_back(value);
        }
        else {
            // search for the equal sign
            eq = el.find('=');
            // this must be the character before the quote
            if (eq == std::string::npos || eq != first - 1) {
                throw cRuntimeError("invalid syntax for mapping \"%s\"", el.c_str());
            }
            else {
                type = el.substr(0, eq);
            }
            mapping.push_back(type);
            mapping.push_back(value);
        }
    }
    return mapping;
}

TraCIScenarioManager::TypeMapping TraCIScenarioManager::parseMappings(std::string parameter, std::string parameterName, bool allowEmpty)
{

    /**
     * possible syntaxes
     *
     * "a"          : assign module type "a" to all nodes (for backward compatibility)
     * "a=b"        : assign module type "b" to vehicle type "a". the presence of any other vehicle type in the simulation will cause the simulation to stop
     * "a=b c=d"    : assign module type "b" to vehicle type "a" and "d" to "c". the presence of any other vehicle type in the simulation will cause the simulation to stop
     * "a=b c=d *=e": everything which is not of vehicle type "a" or "b", assign module type "e"
     * "a=b c=0"    : for vehicle type "c" no module should be instantiated
     * "a=b c=d *=0": everything which is not of vehicle type a or c should not be instantiated
     *
     * For display strings key-value pairs needs to be protected with single quotes, as they use an = sign as the type mappings. For example
     * *.manager.moduleDisplayString = "'i=block/process'"
     * *.manager.moduleDisplayString = "a='i=block/process' b='i=misc/sun'"
     *
     * moduleDisplayString can also be left empty:
     * *.manager.moduleDisplayString = ""
     */

    unsigned int i;
    TypeMapping map;

    // tokenizer to split into mappings ("a=b c=d", -> ["a=b", "c=d"])
    cStringTokenizer typesTz(parameter.c_str(), " ");
    // get all mappings
    std::vector<std::string> typeMappings = typesTz.asVector();
    // and check that there exists at least one
    if (typeMappings.size() == 0) {
        if (!allowEmpty)
            throw cRuntimeError("parameter \"%s\" is empty", parameterName.c_str());
        else
            return map;
    }

    // loop through all mappings
    for (i = 0; i < typeMappings.size(); i++) {

        // tokenizer to find the mapping from vehicle type to module type
        std::string typeMapping = typeMappings[i];

        std::vector<std::string> mapping = getMapping(typeMapping);

        if (mapping.size() == 1) {
            // we are where there is no actual assignment
            // "a": this is good
            // "a b=c": this is not
            if (typeMappings.size() != 1)
                // stop simulation with an error
                throw cRuntimeError("parameter \"%s\" includes multiple mappings, but \"%s\" is not mapped to any vehicle type", parameterName.c_str(), mapping[0].c_str());
            else
                // all vehicle types should be instantiated with this module type
                map["*"] = mapping[0];
        }
        else {

            // check that mapping is valid (a=b and not like a=b=c)
            if (mapping.size() != 2) throw cRuntimeError("invalid syntax for mapping \"%s\" for parameter \"%s\"", typeMapping.c_str(), parameterName.c_str());
            // check that the mapping does not already exist
            if (map.find(mapping[0]) != map.end()) throw cRuntimeError("duplicated mapping for vehicle type \"%s\" for parameter \"%s\"", mapping[0].c_str(), parameterName.c_str());

            // finally save the mapping
            map[mapping[0]] = mapping[1];
        }
    }

    return map;
}

void TraCIScenarioManager::parseModuleTypes()
{

    TypeMapping::iterator i;
    std::vector<std::string> typeKeys, nameKeys, displayStringKeys;

    std::string moduleTypes = par("moduleType").stdstringValue();
    std::string moduleNames = par("moduleName").stdstringValue();
    std::string moduleDisplayStrings = par("moduleDisplayString").stdstringValue();

    moduleType = parseMappings(moduleTypes, "moduleType", false);
    moduleName = parseMappings(moduleNames, "moduleName", false);
    moduleDisplayString = parseMappings(moduleDisplayStrings, "moduleDisplayString", true);

    // perform consistency check. for each vehicle type in moduleType there must be a vehicle type
    // in moduleName (and in moduleDisplayString if moduleDisplayString is not empty)

    // get all the keys
    for (i = moduleType.begin(); i != moduleType.end(); i++) typeKeys.push_back(i->first);
    for (i = moduleName.begin(); i != moduleName.end(); i++) nameKeys.push_back(i->first);
    for (i = moduleDisplayString.begin(); i != moduleDisplayString.end(); i++) displayStringKeys.push_back(i->first);

    // sort them (needed for intersection)
    std::sort(typeKeys.begin(), typeKeys.end());
    std::sort(nameKeys.begin(), nameKeys.end());
    std::sort(displayStringKeys.begin(), displayStringKeys.end());

    std::vector<std::string> intersection;

    // perform set intersection
    std::set_intersection(typeKeys.begin(), typeKeys.end(), nameKeys.begin(), nameKeys.end(), std::back_inserter(intersection));
    if (intersection.size() != typeKeys.size() || intersection.size() != nameKeys.size()) throw cRuntimeError("keys of mappings of moduleType and moduleName are not the same");

    if (displayStringKeys.size() == 0) return;

    intersection.clear();
    std::set_intersection(typeKeys.begin(), typeKeys.end(), displayStringKeys.begin(), displayStringKeys.end(), std::back_inserter(intersection));
    if (intersection.size() != displayStringKeys.size()) throw cRuntimeError("keys of mappings of moduleType and moduleName are not the same");
}

void TraCIScenarioManager::initialize(int stage)
{
    cSimpleModule::initialize(stage);
    if (stage != 1) {
        return;
    }

    trafficLightModuleType = par("trafficLightModuleType").stdstringValue();
    trafficLightModuleName = par("trafficLightModuleName").stdstringValue();
    trafficLightModuleDisplayString = par("trafficLightModuleDisplayString").stdstringValue();
    trafficLightModuleIds.clear();
    std::istringstream filterstream(par("trafficLightFilter").stdstringValue());
    std::copy(std::istream_iterator<std::string>(filterstream), std::istream_iterator<std::string>(), std::back_inserter(trafficLightModuleIds));

    debug = par("debug");
    connectAt = par("connectAt");
    firstStepAt = par("firstStepAt");
    updateInterval = par("updateInterval");
    if (firstStepAt == -1) firstStepAt = connectAt + updateInterval;
    parseModuleTypes();
    penetrationRate = par("penetrationRate").doubleValue();
    ignoreGuiCommands = par("ignoreGuiCommands");
    host = par("host").stdstringValue();
    port = par("port");
    autoShutdown = par("autoShutdown");
    std::string roiRoads_s = par("roiRoads");
    std::string roiRects_s = par("roiRects");

    carCellSize = par("carCellSize").doubleValue();

    vehicleNameCounter = 0;
    vehicleRngIndex = par("vehicleRngIndex");
    numVehicles = par("numVehicles").longValue();
    mobRng = getRNG(vehicleRngIndex);

    annotations = AnnotationManagerAccess().getIfExists();

    // parse roiRoads
    roiRoads.clear();
    std::istringstream roiRoads_i(roiRoads_s);
    std::string road;
    while (std::getline(roiRoads_i, road, ' ')) {
        roiRoads.push_back(road);
    }

    // parse roiRects
    roiRects.clear();
    std::istringstream roiRects_i(roiRects_s);
    std::string rect;
    while (std::getline(roiRects_i, rect, ' ')) {
        std::istringstream rect_i(rect);
        double x1;
        rect_i >> x1;
        ASSERT(rect_i);
        char c1;
        rect_i >> c1;
        ASSERT(rect_i);
        double y1;
        rect_i >> y1;
        ASSERT(rect_i);
        char c2;
        rect_i >> c2;
        ASSERT(rect_i);
        double x2;
        rect_i >> x2;
        ASSERT(rect_i);
        char c3;
        rect_i >> c3;
        ASSERT(rect_i);
        double y2;
        rect_i >> y2;
        ASSERT(rect_i);
        roiRects.push_back(std::pair<TraCICoord, TraCICoord>(TraCICoord(x1, y1), TraCICoord(x2, y2)));
    }

    areaSum = 0;
    nextNodeVectorIndex = 0;
    hostModules.clear();
    subscribedVehicles.clear();
    trafficLights.clear();
    activeVehicleCount = 0;
    parkingVehicleCount = 0;
    drivingVehicleCount = 0;
    autoShutdownTriggered = false;

    world = FindModule<BaseWorldUtility*>::findGlobalModule();

    cc = FindModule<BaseConnectionManager*>::findGlobalModule();

    // determine the size of the car grid based on the playground size and the grid granularity
    if (carCellSize != 0.0) {
        carGridCols = (int) ceil(world->getPgs()->x / carCellSize);
        carGridRows = (int) ceil(world->getPgs()->y / carCellSize);
    }
    else {
        carGridCols = 1;
        carGridRows = 1;
    }
    // use 1D array for storage and initialize with empty maps
    int n = carGridCols * carGridRows;
    hostsGrid = new std::map<std::string, HostPos*>[n];
    std::fill_n(hostsGrid, n, std::map<std::string, HostPos*>());

    ASSERT(firstStepAt > connectAt);
    connectAndStartTrigger = new cMessage("connect");
    scheduleAt(connectAt, connectAndStartTrigger);
    executeOneTimestepTrigger = new cMessage("step");
    scheduleAt(firstStepAt, executeOneTimestepTrigger);

    EV_DEBUG << "initialized TraCIScenarioManager" << endl;
}

void TraCIScenarioManager::init_traci()
{
    auto* commandInterface = getCommandInterface();
    {
        auto apiVersion = commandInterface->getVersion();
        EV_DEBUG << "TraCI server \"" << apiVersion.second << "\" reports API version " << apiVersion.first << endl;
        commandInterface->setApiVersion(apiVersion.first);
    }

    {
        // query and set road network boundaries
        auto networkBoundaries = commandInterface->initNetworkBoundaries(par("margin"));
        if (world != nullptr && ((connection->traci2omnet(networkBoundaries.second).x > world->getPgs()->x) || (connection->traci2omnet(networkBoundaries.first).y > world->getPgs()->y))) {
            EV_WARN << "WARNING: Playground size (" << world->getPgs()->x << ", " << world->getPgs()->y << ") might be too small for vehicle at network bounds (" << connection->traci2omnet(networkBoundaries.second).x << ", " << connection->traci2omnet(networkBoundaries.first).y << ")" << endl;
        }
    }

    {
        // subscribe to list of departed and arrived vehicles, as well as simulation time
        simtime_t beginTime = 0;
        simtime_t endTime = SimTime::getMaxTime();
        std::string objectId = "";
        uint8_t variableNumber = 7;
        uint8_t variable1 = VAR_DEPARTED_VEHICLES_IDS;
        uint8_t variable2 = VAR_ARRIVED_VEHICLES_IDS;
        uint8_t variable3 = commandInterface->getTimeStepCmd();
        uint8_t variable4 = VAR_TELEPORT_STARTING_VEHICLES_IDS;
        uint8_t variable5 = VAR_TELEPORT_ENDING_VEHICLES_IDS;
        uint8_t variable6 = VAR_PARKING_STARTING_VEHICLES_IDS;
        uint8_t variable7 = VAR_PARKING_ENDING_VEHICLES_IDS;
        TraCIBuffer buf = connection->query(CMD_SUBSCRIBE_SIM_VARIABLE, TraCIBuffer() << beginTime << endTime << objectId << variableNumber << variable1 << variable2 << variable3 << variable4 << variable5 << variable6 << variable7);
        processSubcriptionResult(buf);
        ASSERT(buf.eof());
    }

    {
        // subscribe to list of vehicle ids
        simtime_t beginTime = 0;
        simtime_t endTime = SimTime::getMaxTime();
        std::string objectId = "";
        uint8_t variableNumber = 1;
        uint8_t variable1 = ID_LIST;
        TraCIBuffer buf = connection->query(CMD_SUBSCRIBE_VEHICLE_VARIABLE, TraCIBuffer() << beginTime << endTime << objectId << variableNumber << variable1);
        processSubcriptionResult(buf);
        ASSERT(buf.eof());
    }

    if (!trafficLightModuleType.empty() && !trafficLightModuleIds.empty()) {
        // initialize traffic lights
        cModule* parentmod = getParentModule();
        if (!parentmod) {
            throw cRuntimeError("Parent Module not found (for traffic light creation)");
        }
        cModuleType* tlModuleType = cModuleType::get(trafficLightModuleType.c_str());

        // query traffic lights via TraCI
        std::list<std::string> trafficLightIds = commandInterface->getTrafficlightIds();
        size_t nrOfTrafficLights = trafficLightIds.size();
        int cnt = 0;
        for (std::list<std::string>::iterator i = trafficLightIds.begin(); i != trafficLightIds.end(); ++i) {
            std::string tlId = *i;
            if (std::find(trafficLightModuleIds.begin(), trafficLightModuleIds.end(), tlId) == trafficLightModuleIds.end()) {
                continue; // filter only selected elements
            }

            Coord position = commandInterface->junction(tlId).getPosition();

            cModule* module = tlModuleType->create(trafficLightModuleName.c_str(), parentmod, nrOfTrafficLights, cnt);
            module->par("externalId") = tlId;
            module->finalizeParameters();
            module->getDisplayString().parse(trafficLightModuleDisplayString.c_str());
            module->buildInside();
            module->scheduleStart(simTime() + updateInterval);

            cModule* tlIfSubmodule = module->getSubmodule("tlInterface");
            // initialize traffic light interface with current program
            TraCITrafficLightInterface* tlIfModule = dynamic_cast<TraCITrafficLightInterface*>(tlIfSubmodule);
            tlIfModule->preInitialize(tlId, position, updateInterval);

            // initialize mobility for positioning
            cModule* mobiSubmodule = module->getSubmodule("mobility");
            mobiSubmodule->par("x") = position.x;
            mobiSubmodule->par("y") = position.y;
            mobiSubmodule->par("z") = position.z;

            module->callInitialize();
            trafficLights[tlId] = module;
            subscribeToTrafficLightVariables(tlId); // subscribe after module is in trafficLights
            cnt++;
        }
    }

    ObstacleControl* obstacles = ObstacleControlAccess().getIfExists();
    if (obstacles) {
        {
            // get list of polygons
            std::list<std::string> ids = commandInterface->getPolygonIds();
            for (std::list<std::string>::iterator i = ids.begin(); i != ids.end(); ++i) {
                std::string id = *i;
                std::string typeId = commandInterface->polygon(id).getTypeId();
                if (!obstacles->isTypeSupported(typeId)) continue;
                std::list<Coord> coords = commandInterface->polygon(id).getShape();
                double height = commandInterface->polygon(id).getLayer();
                std::vector<Coord> shape;
                std::copy(coords.begin(), coords.end(), std::back_inserter(shape));
                for (auto p : shape) {
                    if ((p.x < 0) || (p.y < 0) || (p.x > world->getPgs()->x) || (p.y > world->getPgs()->y)) {
                        EV_WARN << "WARNING: Playground (" << world->getPgs()->x << ", " << world->getPgs()->y << ") will not fit radio obstacle at (" << p.x << ", " << p.y << ")" << endl;
                    }
                }
                obstacles->addFromTypeAndShape(id, typeId, shape, height);
            }
        }
    }

    FloorControl* floorControl = FloorControlAccess().getIfExists();
    if (floorControl) {
        floorControl->addXmlSegments(connection);
        std::list<std::string> lanes = getCommandInterface()->getLaneIds();
        for (std::string id : lanes) {
            // TODO: check type similar to obstacles
            //std::string typeId = getCommandInterface()->lane(id).getTypeId();
            //if (!obstacles->isTypeSupported(typeId)) continue;
            std::string roadId = getCommandInterface()->lane(id).getRoadId();
            std::string roadName = getCommandInterface()->road(roadId).getName();

            // only roads marked as floors are considered
            std::string isFloor = "false";
            getCommandInterface()->road(roadId).getParameter("floor", isFloor);
            if (isFloor != "true")
                continue;

            std::list<Coord> coords = getCommandInterface()->lane(id).getShape();
            double laneWidth = getCommandInterface()->lane(id).getWidth();
            std::vector<Coord> shape;
            std::copy(coords.begin(), coords.end(), std::back_inserter(shape));
            floorControl->addLaneFromTypeAndShape(id, "", shape, laneWidth);
        }
        std::list<std::string> junctions = getCommandInterface()->getJunctionIds();
        for (std::string id : junctions) {
            // TODO: check type similar to obstacles
            //std::string typeId = getCommandInterface()->lane(id).getTypeId();
            //if (!obstacles->isTypeSupported(typeId)) continue;

            // only junctions marked as floors are considered
            std::string isFloor = "false";
            getCommandInterface()->junction(id).getParameter("floor", isFloor);
            if (isFloor != "true")
                continue;

            std::list<Coord> coords = getCommandInterface()->junction(id).getShape();
            std::vector<Coord> shape;
            std::copy(coords.begin(), coords.end(), std::back_inserter(shape));
            floorControl->addJunctionFromTypeAndShape(id, "", shape);
        }
    }

    TunnelControl* tunnelControl = TunnelControlAccess().getIfExists();
    if (obstacles && floorControl && tunnelControl) {
        tunnelControl->addFromNetXml(connection); // a more unified way also using TraCI might be desirable
    }

    emit(traciInitializedSignal, true);

    // draw and calculate area of rois
    for (std::list<std::pair<TraCICoord, TraCICoord> >::const_iterator roi = roiRects.begin(), end = roiRects.end();
            roi != end; ++roi) {
        TraCICoord first = roi->first;
        TraCICoord second = roi->second;

        std::list<Coord> pol;

        Coord a = connection->traci2omnet(first);
        Coord b = connection->traci2omnet(TraCICoord(first.x, second.y));
        Coord c = connection->traci2omnet(second);
        Coord d = connection->traci2omnet(TraCICoord(second.x, first.y));

        pol.push_back(a);
        pol.push_back(b);
        pol.push_back(c);
        pol.push_back(d);

        // draw polygon for region of interest
        if (annotations) {
            annotations->drawPolygon(pol, "black");
        }

        // calculate region area
        double ab = a.distance(b);
        double ad = a.distance(d);
        double area = ab * ad;
        areaSum += area;
    }
}

void TraCIScenarioManager::finish()
{
    if (connection) {
        TraCIBuffer buf = connection->query(CMD_CLOSE, TraCIBuffer());
    }
    recordScalar("numVehicles", hostModules.size());
    while (hostModules.begin() != hostModules.end()) {
        deleteManagedModule(hostModules.begin()->first);
    }

    recordScalar("roiArea", areaSum);
}

void TraCIScenarioManager::handleMessage(cMessage* msg)
{
    if (msg->isSelfMessage()) {
        handleSelfMsg(msg);
        return;
    }
    throw cRuntimeError("TraCIScenarioManager doesn't handle messages from other modules");
}

void TraCIScenarioManager::handleSelfMsg(cMessage* msg)
{
    if (msg == connectAndStartTrigger) {
        connection = TraCIConnection::connect(this, host.c_str(), port);
        commandIfc = new TraCICommandInterface(this, *connection, ignoreGuiCommands);
        init_traci();
        return;
    }
    if (msg == executeOneTimestepTrigger) {
        if (simTime() > 1) {
            if (vehicleTypeIds.size() == 0) {
                std::list<std::string> vehTypes = getCommandInterface()->getVehicleTypeIds();
                for (std::list<std::string>::const_iterator i = vehTypes.begin(); i != vehTypes.end(); ++i) {
                    if (i->compare("DEFAULT_VEHTYPE") != 0) {
                        EV_DEBUG << *i << std::endl;
                        vehicleTypeIds.push_back(*i);
                    }
                }
            }
            if (routeIds.size() == 0) {
                std::list<std::string> routes = getCommandInterface()->getRouteIds();
                for (std::list<std::string>::const_iterator i = routes.begin(); i != routes.end(); ++i) {
                    std::string routeId = *i;
                    if (par("useRouteDistributions").boolValue() == true) {
                        if (std::count(routeId.begin(), routeId.end(), '#') >= 1) {
                            EV_DEBUG << "Omitting route " << routeId
                                    << " as it seems to be a member of a route distribution (found '#' in name)"
                                    << std::endl;
                            continue;
                        }
                    }
                    EV_DEBUG << "Adding " << routeId << " to list of possible routes" << std::endl;
                    routeIds.push_back(routeId);
                }
            }
            for (int i = activeVehicleCount + queuedVehicles.size(); i < numVehicles; i++) {
                insertNewVehicle();
            }
        }
        executeOneTimestep();
        return;
    }
    throw cRuntimeError("TraCIScenarioManager received unknown self-message");
}

void TraCIScenarioManager::preInitializeModule(cModule* mod, const std::string& nodeId, const Coord& position,
        const std::string& road_id, double speed, double angle, double elev_angle, VehicleSignal signals)
{
    // pre-initialize TraCIMobility
    for (cModule::SubmoduleIterator iter(mod); !iter.end(); iter++) {
        cModule* submod = SUBMODULE_ITERATOR_TO_MODULE(iter);
        TraCIMobility* mm = dynamic_cast<TraCIMobility*>(submod);
        if (!mm)
            continue;
        mm->preInitialize(nodeId, position, road_id, speed, angle, elev_angle);
    }
}

void TraCIScenarioManager::updateModulePosition(cModule* mod, const Coord& p, const std::string& edge, double speed,
        double angle, double elev_angle, VehicleSignal signals)
{
    // update position in TraCIMobility
    for (cModule::SubmoduleIterator iter(mod); !iter.end(); iter++) {
        cModule* submod = SUBMODULE_ITERATOR_TO_MODULE(iter);
        TraCIMobility* mm = dynamic_cast<TraCIMobility*>(submod);
        if (!mm)
            continue;
        mm->nextPosition(p, edge, speed, angle, elev_angle, signals);
    }
}

// name: host;Car;i=vehicle.gif
void TraCIScenarioManager::addModule(std::string nodeId, std::string type, std::string name, std::string displayString,
        const Coord& position, std::string road_id, double speed, double angle, double elev_angle,
        VehicleSignal signals, double length, double height, double width)
{

    if (hostModules.find(nodeId) != hostModules.end()) throw cRuntimeError("tried adding duplicate module");

    if (queuedVehicles.find(nodeId) != queuedVehicles.end()) {
        queuedVehicles.erase(nodeId);
    }
    double option1 = hostModules.size() / (hostModules.size() + unequippedHostPositions.size() + 1.0);
    double option2 = (hostModules.size() + 1) / (hostModules.size() + unequippedHostPositions.size() + 1.0);

    if (fabs(option1 - penetrationRate) < fabs(option2 - penetrationRate)) {
        addToHostPosMap(unequippedHostPositions, nodeId, position, angle, elev_angle);
        return;
    }

    int32_t nodeVectorIndex = nextNodeVectorIndex++;

    cModule* parentmod = getParentModule();
    if (!parentmod) throw cRuntimeError("Parent Module not found");

    cModuleType* nodeType = cModuleType::get(type.c_str());
    if (!nodeType) throw cRuntimeError("Module Type \"%s\" not found", type.c_str());

    // TODO: this trashes the vectsize member of the cModule, although nobody seems to use it
    cModule* mod = nodeType->create(name.c_str(), parentmod, nodeVectorIndex, nodeVectorIndex);
    mod->finalizeParameters();
    if (displayString.length() > 0) {
        mod->getDisplayString().parse(displayString.c_str());
    }
    mod->buildInside();
    mod->scheduleStart(simTime() + updateInterval);

    preInitializeModule(mod, nodeId, position, road_id, speed, angle, elev_angle, signals);

    mod->callInitialize();
    addToHostPosMap(equippedHostPositions, nodeId, position, angle, elev_angle);
    hostModules[nodeId] = mod;

    // post-initialize TraCIMobility
    for (cModule::SubmoduleIterator iter(mod); !iter.end(); iter++) {
        cModule* submod = SUBMODULE_ITERATOR_TO_MODULE(iter);
        TraCIMobility* mm = dynamic_cast<TraCIMobility*>(submod);
        if (!mm)
            continue;
        mm->changePosition();
    }
}

cModule* TraCIScenarioManager::getManagedModule(std::string nodeId)
{
    if (hostModules.find(nodeId) == hostModules.end())
        return 0;
    return hostModules[nodeId];
}

bool TraCIScenarioManager::isModuleUnequipped(std::string nodeId)
{
    if (unequippedHostPositions.find(nodeId) == unequippedHostPositions.end())
        return false;
    return true;
}

void TraCIScenarioManager::addToHostPosMap(std::map<std::string, HostPos>& hostPosMap, std::string nodeId,
        const Coord& position, double angle, double elev_angle)
{
    hostPosMap[nodeId] = std::make_tuple(position,
            Coord(cos(elev_angle) * cos(angle), -cos(elev_angle) * sin(angle), sin(elev_angle)),
            std::vector<GridCoord>());
    std::vector<GridCoord>& cells = std::get < 2 > (hostPosMap[nodeId]);
    size_t fromRow = 0, toRow = 0, fromCol = 0, toCol = 0;
    if (carCellSize != 0.0) {
        fromRow = std::max(0, (int) ((position.y - LENGTH) / carCellSize));
        toRow = std::min((int) (world->getPgs()->y / carCellSize), (int) ((position.y + LENGTH) / carCellSize));
        fromCol = std::max(0, (int) ((position.x - LENGTH) / carCellSize));
        toCol = std::min((int) (world->getPgs()->x / carCellSize), (int) ((position.x + LENGTH) / carCellSize));
    }
    for (size_t row = fromRow; row <= toRow; ++row) {
        for (size_t col = fromCol; col <= toCol; ++col) {
            (hostsGrid[row * carGridCols + col])[nodeId] = &hostPosMap[nodeId];
            cells.push_back(GridCoord(col, row));
        }
    }
}

void TraCIScenarioManager::updateHostPosMap(std::map<std::string, HostPos>& hostPosMap, std::string nodeId,
        const Coord& position, double angle, double elev_angle)
{
    HostPos& hostTuple = hostPosMap[nodeId];
    std::get < 0 > (hostTuple) = position;
    std::get < 1 > (hostTuple) = Coord(cos(elev_angle) * cos(angle), -cos(elev_angle) * sin(angle), sin(elev_angle));
    std::vector<GridCoord>& cells = std::get < 2 > (hostTuple);
    for (GridCoord const g : cells) {
        (hostsGrid[g.y * carGridCols + g.x]).erase(nodeId);
    }
    cells.clear();
    size_t fromRow = 0, toRow = 0, fromCol = 0, toCol = 0;
    if (carCellSize != 0.0) {
        fromRow = std::max(0, (int) ((position.y - LENGTH) / carCellSize));
        toRow = std::min((int) (world->getPgs()->y / carCellSize), (int) ((position.y + LENGTH) / carCellSize));
        fromCol = std::max(0, (int) ((position.x - LENGTH) / carCellSize));
        toCol = std::min((int) (world->getPgs()->x / carCellSize), (int) ((position.x + LENGTH) / carCellSize));
    }
    for (size_t row = fromRow; row <= toRow; ++row) {
        for (size_t col = fromCol; col <= toCol; ++col) {
            (hostsGrid[row * carGridCols + col])[nodeId] = &hostTuple;
            cells.push_back(GridCoord(col, row));
        }
    }
}

void TraCIScenarioManager::eraseFromHostPosMap(std::map<std::string, HostPos>& hostPosMap, std::string nodeId)
{
    std::vector<GridCoord>& cells = std::get < 2 > (hostPosMap[nodeId]);
    for (GridCoord const g : cells) {
        (hostsGrid[g.y * carGridCols + g.x]).erase(nodeId);
    }
    hostPosMap.erase(nodeId);
}

void TraCIScenarioManager::deleteManagedModule(std::string nodeId)
{
    cModule* mod = getManagedModule(nodeId);
    if (!mod)
        error("no vehicle with Id \"%s\" found", nodeId.c_str());

    cModule* nic = mod->getSubmodule("nic");
    if (cc && nic) {
        cc->unregisterNic(nic);
    }

    hostModules.erase(nodeId);
    eraseFromHostPosMap(equippedHostPositions, nodeId);
    mod->callFinish();
    mod->deleteModule();
}

bool TraCIScenarioManager::isInRegionOfInterest(const TraCICoord& position, std::string road_id, double speed,
        double angle)
{
    if ((roiRoads.size() == 0) && (roiRects.size() == 0))
        return true;
    if (roiRoads.size() > 0) {
        for (std::list<std::string>::const_iterator i = roiRoads.begin(); i != roiRoads.end(); ++i) {
            if (road_id == *i)
                return true;
        }
    }
    if (roiRects.size() > 0) {
        for (std::list<std::pair<TraCICoord, TraCICoord> >::const_iterator i = roiRects.begin(); i != roiRects.end();
                ++i) {
            if ((position.x >= i->first.x) && (position.y >= i->first.y) && (position.x <= i->second.x)
                    && (position.y <= i->second.y))
                return true;
        }
    }
    return false;
}

void TraCIScenarioManager::executeOneTimestep()
{

    EV_DEBUG << "Triggering TraCI server simulation advance to t=" << simTime() << endl;

    simtime_t targetTime = simTime();

    if (isConnected()) {
        insertVehicles();
        TraCIBuffer buf = connection->query(CMD_SIMSTEP2, TraCIBuffer() << targetTime);

        uint32_t count;
        buf >> count;
        EV_DEBUG << "Getting " << count << " subscription results" << endl;
        for (uint32_t i = 0; i < count; ++i) {
            processSubcriptionResult(buf);
        }
    }

    if (!autoShutdownTriggered) scheduleAt(simTime() + updateInterval, executeOneTimestepTrigger);

}

void TraCIScenarioManager::insertNewVehicle()
{
    std::string type;
    if (vehicleTypeIds.size()) {
        int vehTypeId = mobRng->intRand(vehicleTypeIds.size());
        type = vehicleTypeIds[vehTypeId];
    }
    else {
        type = "DEFAULT_VEHTYPE";
    }
    int routeId = mobRng->intRand(routeIds.size());
    vehicleInsertQueue[routeId].push(type);
}

void TraCIScenarioManager::insertVehicles()
{

    for (std::map<int, std::queue<std::string> >::iterator i = vehicleInsertQueue.begin();
            i != vehicleInsertQueue.end();) {
        std::string route = routeIds[i->first];
        EV_DEBUG << "process " << route << std::endl;
        std::queue<std::string> vehicles = i->second;
        while (!i->second.empty()) {
            bool suc = false;
            std::string type = i->second.front();
            std::stringstream veh;
            veh << type << "_" << vehicleNameCounter;
            EV_DEBUG << "trying to add " << veh.str() << " with " << route << " vehicle type " << type << std::endl;

            suc = getCommandInterface()->addVehicle(veh.str(), type, route, simTime());
            if (!suc) {
                i->second.pop();
            }
            else {
                EV_DEBUG << "successful inserted " << veh.str() << std::endl;
                queuedVehicles.insert(veh.str());
                i->second.pop();
                vehicleNameCounter++;
            }
        }
        std::map<int, std::queue<std::string> >::iterator tmp = i;
        ++tmp;
        vehicleInsertQueue.erase(i);
        i = tmp;

    }
}

void TraCIScenarioManager::subscribeToVehicleVariables(std::string vehicleId)
{
    // subscribe to some attributes of the vehicle
    simtime_t beginTime = 0;
    simtime_t endTime = SimTime::getMaxTime();
    std::string objectId = vehicleId;
    uint8_t variableNumber = 9;
    uint8_t variable1 = VAR_POSITION3D;
    uint8_t variable2 = VAR_ROAD_ID;
    uint8_t variable3 = VAR_SPEED;
    uint8_t variable4 = VAR_ANGLE;
    uint8_t variable5 = VAR_SIGNALS;
    uint8_t variable6 = VAR_LENGTH;
    uint8_t variable7 = VAR_HEIGHT;
    uint8_t variable8 = VAR_WIDTH;
    uint8_t variable9 = VAR_SLOPE;

    TraCIBuffer buf = connection->query(CMD_SUBSCRIBE_VEHICLE_VARIABLE,
            TraCIBuffer() << beginTime << endTime << objectId << variableNumber << variable1 << variable2 << variable3
                    << variable4 << variable5 << variable6 << variable7 << variable8 << variable9);
    processSubcriptionResult(buf);
    ASSERT(buf.eof());
}

void TraCIScenarioManager::unsubscribeFromVehicleVariables(std::string vehicleId)
{
    // subscribe to some attributes of the vehicle
    simtime_t beginTime = 0;
    simtime_t endTime = SimTime::getMaxTime();
    std::string objectId = vehicleId;
    uint8_t variableNumber = 0;

    TraCIBuffer buf = connection->query(CMD_SUBSCRIBE_VEHICLE_VARIABLE, TraCIBuffer() << beginTime << endTime << objectId << variableNumber);
    ASSERT(buf.eof());
}

void TraCIScenarioManager::subscribeToTrafficLightVariables(std::string tlId)
{
    // subscribe to some attributes of the traffic light system
    simtime_t beginTime = 0;
    simtime_t endTime = SimTime::getMaxTime();
    std::string objectId = tlId;
    uint8_t variableNumber = 4;
    uint8_t variable1 = TL_CURRENT_PHASE;
    uint8_t variable2 = TL_CURRENT_PROGRAM;
    uint8_t variable3 = TL_NEXT_SWITCH;
    uint8_t variable4 = TL_RED_YELLOW_GREEN_STATE;

    TraCIBuffer buf = connection->query(CMD_SUBSCRIBE_TL_VARIABLE, TraCIBuffer() << beginTime << endTime << objectId << variableNumber << variable1 << variable2 << variable3 << variable4);
    processSubcriptionResult(buf);
    ASSERT(buf.eof());
}

void TraCIScenarioManager::unsubscribeFromTrafficLightVariables(std::string tlId)
{
    // unsubscribe from some attributes of the traffic light system
    // this method is mainly for completeness as traffic lights are not supposed to be removed at runtime

    simtime_t beginTime = 0;
    simtime_t endTime = SimTime::getMaxTime();
    std::string objectId = tlId;
    uint8_t variableNumber = 0;

    TraCIBuffer buf = connection->query(CMD_SUBSCRIBE_TL_VARIABLE, TraCIBuffer() << beginTime << endTime << objectId << variableNumber);
    ASSERT(buf.eof());
}

void TraCIScenarioManager::processTrafficLightSubscription(std::string objectId, TraCIBuffer& buf)
{
    cModule* tlIfSubmodule = trafficLights[objectId]->getSubmodule("tlInterface");
    TraCITrafficLightInterface* tlIfModule = dynamic_cast<TraCITrafficLightInterface*>(tlIfSubmodule);
    if (!tlIfModule) {
        throw cRuntimeError("Could not find traffic light module %s", objectId.c_str());
    }

    uint8_t variableNumber_resp;
    buf >> variableNumber_resp;
    for (uint8_t j = 0; j < variableNumber_resp; ++j) {
        uint8_t response_type;
        buf >> response_type;
        uint8_t isokay;
        buf >> isokay;
        if (isokay != RTYPE_OK) {
            std::string description = buf.readTypeChecked<std::string>(TYPE_STRING);
            if (isokay == RTYPE_NOTIMPLEMENTED) {
                throw cRuntimeError("TraCI server reported subscribing to 0x%2x not implemented (\"%s\"). Might need newer version.", response_type, description.c_str());
            }
            else {
                throw cRuntimeError("TraCI server reported error subscribing to variable 0x%2x (\"%s\").", response_type, description.c_str());
            }
        }
        switch (response_type) {
        case TL_CURRENT_PHASE:
            tlIfModule->setCurrentPhaseByNr(buf.readTypeChecked<int32_t>(TYPE_INTEGER), false);
            break;

        case TL_CURRENT_PROGRAM:
            tlIfModule->setCurrentLogicById(buf.readTypeChecked<std::string>(TYPE_STRING), false);
            break;

        case TL_NEXT_SWITCH:
            tlIfModule->setNextSwitch(buf.readTypeChecked<simtime_t>(getCommandInterface()->getTimeType()), false);
            break;

        case TL_RED_YELLOW_GREEN_STATE:
            tlIfModule->setCurrentState(buf.readTypeChecked<std::string>(TYPE_STRING), false);
            break;

        default:
            throw cRuntimeError("Received unhandled traffic light subscription result; type: 0x%02x", response_type);
            break;
        }
    }
}

void TraCIScenarioManager::processSimSubscription(std::string objectId, TraCIBuffer& buf)
{
    uint8_t variableNumber_resp;
    buf >> variableNumber_resp;
    for (uint8_t j = 0; j < variableNumber_resp; ++j) {
        uint8_t variable1_resp;
        buf >> variable1_resp;
        uint8_t isokay;
        buf >> isokay;
        if (isokay != RTYPE_OK) {
            uint8_t varType;
            buf >> varType;
            ASSERT(varType == TYPE_STRING);
            std::string description;
            buf >> description;
            if (isokay == RTYPE_NOTIMPLEMENTED) throw cRuntimeError("TraCI server reported subscribing to variable 0x%2x not implemented (\"%s\"). Might need newer version.", variable1_resp, description.c_str());
            throw cRuntimeError("TraCI server reported error subscribing to variable 0x%2x (\"%s\").", variable1_resp, description.c_str());
        }

        if (variable1_resp == VAR_DEPARTED_VEHICLES_IDS) {
            uint8_t varType;
            buf >> varType;
            ASSERT(varType == TYPE_STRINGLIST);
            uint32_t count;
            buf >> count;
            EV_DEBUG << "TraCI reports " << count << " departed vehicles." << endl;
            for (uint32_t i = 0; i < count; ++i) {
                std::string idstring;
                buf >> idstring;
                // adding modules is handled on the fly when entering/leaving the ROI
            }

            activeVehicleCount += count;
            drivingVehicleCount += count;
        }
        else if (variable1_resp == VAR_ARRIVED_VEHICLES_IDS) {
            uint8_t varType;
            buf >> varType;
            ASSERT(varType == TYPE_STRINGLIST);
            uint32_t count;
            buf >> count;
            EV_DEBUG << "TraCI reports " << count << " arrived vehicles." << endl;
            for (uint32_t i = 0; i < count; ++i) {
                std::string idstring;
                buf >> idstring;

                if (subscribedVehicles.find(idstring) != subscribedVehicles.end()) {
                    subscribedVehicles.erase(idstring);
                    // no unsubscription via TraCI possible/necessary as of SUMO 1.0.0 (the vehicle has arrived)
                }

                // check if this object has been deleted already (e.g. because it was outside the ROI)
                cModule* mod = getManagedModule(idstring);
                if (mod) deleteManagedModule(idstring);

                if (unequippedHostPositions.find(idstring) != unequippedHostPositions.end()) {
                    eraseFromHostPosMap(unequippedHostPositions, idstring);
                }
            }

            if ((count > 0) && (count >= activeVehicleCount) && autoShutdown) autoShutdownTriggered = true;
            activeVehicleCount -= count;
            drivingVehicleCount -= count;
        }
        else if (variable1_resp == VAR_TELEPORT_STARTING_VEHICLES_IDS) {
            uint8_t varType;
            buf >> varType;
            ASSERT(varType == TYPE_STRINGLIST);
            uint32_t count;
            buf >> count;
            EV_DEBUG << "TraCI reports " << count << " vehicles starting to teleport." << endl;
            for (uint32_t i = 0; i < count; ++i) {
                std::string idstring;
                buf >> idstring;

                // check if this object has been deleted already (e.g. because it was outside the ROI)
                cModule* mod = getManagedModule(idstring);
                if (mod) deleteManagedModule(idstring);

                if (unequippedHostPositions.find(idstring) != unequippedHostPositions.end()) {
                    eraseFromHostPosMap(unequippedHostPositions, idstring);
                }
            }

            activeVehicleCount -= count;
            drivingVehicleCount -= count;
        }
        else if (variable1_resp == VAR_TELEPORT_ENDING_VEHICLES_IDS) {
            uint8_t varType;
            buf >> varType;
            ASSERT(varType == TYPE_STRINGLIST);
            uint32_t count;
            buf >> count;
            EV_DEBUG << "TraCI reports " << count << " vehicles ending teleport." << endl;
            for (uint32_t i = 0; i < count; ++i) {
                std::string idstring;
                buf >> idstring;
                // adding modules is handled on the fly when entering/leaving the ROI
            }

            activeVehicleCount += count;
            drivingVehicleCount += count;
        }
        else if (variable1_resp == VAR_PARKING_STARTING_VEHICLES_IDS) {
            uint8_t varType;
            buf >> varType;
            ASSERT(varType == TYPE_STRINGLIST);
            uint32_t count;
            buf >> count;
            EV_DEBUG << "TraCI reports " << count << " vehicles starting to park." << endl;
            for (uint32_t i = 0; i < count; ++i) {
                std::string idstring;
                buf >> idstring;

                cModule* mod = getManagedModule(idstring);
                for (cModule::SubmoduleIterator iter(mod); !iter.end(); iter++) {
                    cModule* submod = SUBMODULE_ITERATOR_TO_MODULE(iter);
                    TraCIMobility* mm = dynamic_cast<TraCIMobility*>(submod);
                    if (!mm)
                        continue;
                    mm->changeParkingState(true);
                }
            }

            parkingVehicleCount += count;
            drivingVehicleCount -= count;
        }
        else if (variable1_resp == VAR_PARKING_ENDING_VEHICLES_IDS) {
            uint8_t varType;
            buf >> varType;
            ASSERT(varType == TYPE_STRINGLIST);
            uint32_t count;
            buf >> count;
            EV_DEBUG << "TraCI reports " << count << " vehicles ending to park." << endl;
            for (uint32_t i = 0; i < count; ++i) {
                std::string idstring;
                buf >> idstring;

                cModule* mod = getManagedModule(idstring);
                for (cModule::SubmoduleIterator iter(mod); !iter.end(); iter++) {
                    cModule* submod = SUBMODULE_ITERATOR_TO_MODULE(iter);
                    TraCIMobility* mm = dynamic_cast<TraCIMobility*>(submod);
                    if (!mm)
                        continue;
                    mm->changeParkingState(false);
                }
            }
            parkingVehicleCount -= count;
            drivingVehicleCount += count;
        }
        else if (variable1_resp == getCommandInterface()->getTimeStepCmd()) {
            uint8_t varType;
            buf >> varType;
            ASSERT(varType == getCommandInterface()->getTimeType());
            simtime_t serverTimestep;
            buf >> serverTimestep;
            EV_DEBUG << "TraCI reports current time step as " << serverTimestep << " s." << endl;
            simtime_t omnetTimestep = simTime();
            ASSERT(omnetTimestep == serverTimestep);
        }
        else {
            throw cRuntimeError("Received unhandled sim subscription result");
        }
    }
}

void TraCIScenarioManager::processVehicleSubscription(std::string objectId, TraCIBuffer& buf)
{
    bool isSubscribed = (subscribedVehicles.find(objectId) != subscribedVehicles.end());
    double px;
    double py;
    double pz;
    std::string edge;
    double speed;
    double angle_traci;
    double elev_angle;
    int signals;
    double length;
    double height;
    double width;
    int numRead = 0;

    uint8_t variableNumber_resp;
    buf >> variableNumber_resp;
    for (uint8_t j = 0; j < variableNumber_resp; ++j) {
        uint8_t variable1_resp;
        buf >> variable1_resp;
        uint8_t isokay;
        buf >> isokay;
        if (isokay != RTYPE_OK) {
            uint8_t varType;
            buf >> varType;
            ASSERT(varType == TYPE_STRING);
            std::string errormsg;
            buf >> errormsg;
            if (isSubscribed) {
                if (isokay == RTYPE_NOTIMPLEMENTED) throw cRuntimeError("TraCI server reported subscribing to vehicle variable 0x%2x not implemented (\"%s\"). Might need newer version.", variable1_resp, errormsg.c_str());
                throw cRuntimeError("TraCI server reported error subscribing to vehicle variable 0x%2x (\"%s\").", variable1_resp, errormsg.c_str());
            }
        }
        else if (variable1_resp == ID_LIST) {
            uint8_t varType;
            buf >> varType;
            ASSERT(varType == TYPE_STRINGLIST);
            uint32_t count;
            buf >> count;
            EV_DEBUG << "TraCI reports " << count << " active vehicles." << endl;
            ASSERT(count == activeVehicleCount);
            std::set<std::string> drivingVehicles;
            for (uint32_t i = 0; i < count; ++i) {
                std::string idstring;
                buf >> idstring;
                drivingVehicles.insert(idstring);
            }

            // check for vehicles that need subscribing to
            std::set<std::string> needSubscribe;
            std::set_difference(drivingVehicles.begin(), drivingVehicles.end(), subscribedVehicles.begin(), subscribedVehicles.end(), std::inserter(needSubscribe, needSubscribe.begin()));
            for (std::set<std::string>::const_iterator i = needSubscribe.begin(); i != needSubscribe.end(); ++i) {
                subscribedVehicles.insert(*i);
                subscribeToVehicleVariables(*i);
            }

            // check for vehicles that need unsubscribing from
            std::set<std::string> needUnsubscribe;
            std::set_difference(subscribedVehicles.begin(), subscribedVehicles.end(), drivingVehicles.begin(), drivingVehicles.end(), std::inserter(needUnsubscribe, needUnsubscribe.begin()));
            for (std::set<std::string>::const_iterator i = needUnsubscribe.begin(); i != needUnsubscribe.end(); ++i) {
                subscribedVehicles.erase(*i);
                unsubscribeFromVehicleVariables(*i);
            }
        }
        else if (variable1_resp == VAR_POSITION3D) {
            uint8_t varType;
            buf >> varType;
            ASSERT(varType == POSITION_3D);
            buf >> px;
            buf >> py;
            buf >> pz;
            numRead++;
        }
        else if (variable1_resp == VAR_ROAD_ID) {
            uint8_t varType;
            buf >> varType;
            ASSERT(varType == TYPE_STRING);
            buf >> edge;
            numRead++;
        }
        else if (variable1_resp == VAR_SPEED) {
            uint8_t varType;
            buf >> varType;
            ASSERT(varType == TYPE_DOUBLE);
            buf >> speed;
            numRead++;
        }
        else if (variable1_resp == VAR_ANGLE) {
            uint8_t varType;
            buf >> varType;
            ASSERT(varType == TYPE_DOUBLE);
            buf >> angle_traci;
            numRead++;
        }
        else if (variable1_resp == VAR_SIGNALS) {
            uint8_t varType;
            buf >> varType;
            ASSERT(varType == TYPE_INTEGER);
            buf >> signals;
            numRead++;
        }
        else if (variable1_resp == VAR_LENGTH) {
            uint8_t varType;
            buf >> varType;
            ASSERT(varType == TYPE_DOUBLE);
            buf >> length;
            numRead++;
        }
        else if (variable1_resp == VAR_HEIGHT) {
            uint8_t varType;
            buf >> varType;
            ASSERT(varType == TYPE_DOUBLE);
            buf >> height;
            numRead++;
        }
        else if (variable1_resp == VAR_WIDTH) {
            uint8_t varType;
            buf >> varType;
            ASSERT(varType == TYPE_DOUBLE);
            buf >> width;
            numRead++;
        }
        else if (variable1_resp == VAR_SLOPE) {
            uint8_t varType;
            buf >> varType;
            ASSERT(varType == TYPE_DOUBLE);
            buf >> elev_angle;
            numRead++;
        }
        else {
            throw cRuntimeError("Received unhandled vehicle subscription result");
        }
    }

    // bail out if we didn't want to receive these subscription results
    if (!isSubscribed) return;

    // make sure we got updates for all attributes
    if (numRead != 9)
        return;

    Coord p = connection->traci2omnet(TraCICoord(px, py, pz));
    if ((p.x < 0) || (p.y < 0)) throw cRuntimeError("received bad node position (%.2f, %.2f), translated to (%.2f, %.2f)", px, py, p.x, p.y);

    double angle = connection->traci2omnetAngle(angle_traci);
    elev_angle = elev_angle * M_PI / 180.0;

    cModule* mod = getManagedModule(objectId);

    // is it in the ROI?
    bool inRoi = isInRegionOfInterest(TraCICoord(px, py), edge, speed, angle);
    if (!inRoi) {
        if (mod) {
            deleteManagedModule(objectId);
            EV_DEBUG << "Vehicle #" << objectId << " left region of interest" << endl;
        }
        else if (unequippedHostPositions.find(objectId) != unequippedHostPositions.end()) {
            eraseFromHostPosMap(unequippedHostPositions, objectId);
            EV_DEBUG << "Vehicle (unequipped) # " << objectId << " left region of interest" << endl;
        }
        return;
    }

    if (isModuleUnequipped(objectId)) {
        //if (carCellSize != 0.0)
            updateHostPosMap(unequippedHostPositions, objectId, p, angle, elev_angle);
        return;
    }

    if (!mod) {
        // no such module - need to create
        std::string vType = commandIfc->vehicle(objectId).getTypeId();
        std::string mType, mName, mDisplayString;
        TypeMapping::iterator iType, iName, iDisplayString;

        TypeMapping::iterator i;
        iType = moduleType.find(vType);
        if (iType == moduleType.end()) {
            iType = moduleType.find("*");
            if (iType == moduleType.end()) throw cRuntimeError("cannot find a module type for vehicle type \"%s\"", vType.c_str());
        }
        mType = iType->second;
        // search for module name
        iName = moduleName.find(vType);
        if (iName == moduleName.end()) {
            iName = moduleName.find(std::string("*"));
            if (iName == moduleName.end()) throw cRuntimeError("cannot find a module name for vehicle type \"%s\"", vType.c_str());
        }
        mName = iName->second;
        if (moduleDisplayString.size() != 0) {
            iDisplayString = moduleDisplayString.find(vType);
            if (iDisplayString == moduleDisplayString.end()) {
                iDisplayString = moduleDisplayString.find("*");
                if (iDisplayString == moduleDisplayString.end()) throw cRuntimeError("cannot find a module display string for vehicle type \"%s\"", vType.c_str());
            }
            mDisplayString = iDisplayString->second;
        }
        else {
            mDisplayString = "";
        }

        if (mType != "0") {
            addModule(objectId, mType, mName, mDisplayString, p, edge, speed, angle, elev_angle, VehicleSignal(signals), length, height, width);
            EV_DEBUG << "Added vehicle #" << objectId << endl;
        }
    }
    else {
        // module existed - update position
        EV_DEBUG << "module " << objectId << " moving to " << p.x << "," << p.y << "," << p.z << endl;
        //if (carCellSize != 0.0)
            updateHostPosMap(equippedHostPositions, objectId, p, angle, elev_angle);
        updateModulePosition(mod, p, edge, speed, angle, elev_angle, VehicleSignal(signals));
    }
}

void TraCIScenarioManager::processSubcriptionResult(TraCIBuffer& buf)
{
    uint8_t cmdLength_resp;
    buf >> cmdLength_resp;
    uint32_t cmdLengthExt_resp;
    buf >> cmdLengthExt_resp;
    uint8_t commandId_resp;
    buf >> commandId_resp;
    std::string objectId_resp;
    buf >> objectId_resp;

    if (commandId_resp == RESPONSE_SUBSCRIBE_VEHICLE_VARIABLE)
        processVehicleSubscription(objectId_resp, buf);
    else if (commandId_resp == RESPONSE_SUBSCRIBE_SIM_VARIABLE)
        processSimSubscription(objectId_resp, buf);
    else if (commandId_resp == RESPONSE_SUBSCRIBE_TL_VARIABLE)
        processTrafficLightSubscription(objectId_resp, buf);
    else {
        throw cRuntimeError("Received unhandled subscription result");
    }
}

