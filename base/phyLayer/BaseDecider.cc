/*
 * BaseDecider.cc
 *
 *  Created on: 24.02.2009
 *      Author: karl
 */

#include "BaseDecider.h"

simtime_t BaseDecider::processSignal(AirFrame* frame) {

	assert(frame);
	debugEV << "Processing AirFrame..." << endl;

	switch(getSignalState(frame)) {
	case NEW:
		return processNewSignal(frame);
	case EXPECT_HEADER:
		return processSignalHeader(frame);
	case EXPECT_END:
		return processSignalEnd(frame);
	default:
		return processUnknownSignal(frame);
	}
}

simtime_t BaseDecider::processNewSignal(AirFrame* frame) {
	if(currentSignal.first != 0) {
		debugEV << "Already receiving another AirFrame!" << endl;
		return notAgain;
	}

	// get the receiving power of the Signal at start-time
	Signal& signal = frame->getSignal();
	double recvPower = signal.getReceivingPower()->getValue(Argument(signal.getSignalStart()));

	// check whether signal is strong enough to receive
	if ( recvPower < sensitivity )
	{
		debugEV << "Signal is to weak (" << recvPower << " < " << sensitivity
				<< ") -> do not receive." << endl;
		// Signal too weak, we can't receive it, tell PhyLayer that we don't want it again
		return notAgain;
	}

	// Signal is strong enough, receive this Signal and schedule it
	debugEV << "Signal is strong enough (" << recvPower << " > " << sensitivity
			<< ") -> Trying to receive AirFrame." << endl;

	currentSignal.first = frame;
	currentSignal.second = EXPECT_END;

	//channel turned busy
	setChannelIdleStatus(false);

	return ( signal.getSignalStart() + signal.getSignalLength() );
}

simtime_t BaseDecider::processSignalEnd(AirFrame* frame) {
	EV << "packet was received correctly, it is now handed to upper layer...\n";
	phy->sendUp(frame, new DeciderResult(true));

	// we have processed this AirFrame and we prepare to receive the next one
	currentSignal.first = 0;

	//channel is idle now
	setChannelIdleStatus(true);

	return notAgain;
}

ChannelState BaseDecider::getChannelState() {

	simtime_t now = phy->getSimTime();
	double rssiValue = calcChannelSenseRSSI(now, now);

	return ChannelState(isChannelIdle, rssiValue);
}


simtime_t BaseDecider::handleChannelSenseRequest(ChannelSenseRequest* request) {

	assert(request);

	if (currentChannelSenseRequest.first == 0)
	{
		return handleNewSenseRequest(request);
	}

	if (currentChannelSenseRequest.first != request) {
		opp_error("Got a new ChannelSenseRequest while already handling another one!");
		return notAgain;
	}

	handleSenseRequestTimeout(currentChannelSenseRequest);

	// say that we don't want to have it again
	return notAgain;
}

simtime_t BaseDecider::handleNewSenseRequest(ChannelSenseRequest* request)
{
	// no request handled at the moment, handling the new one
	simtime_t now = phy->getSimTime();

	// saving the pointer to the request and its start-time (now)
	currentChannelSenseRequest.first = request;
	currentChannelSenseRequest.second = now;

	//check if we can already answer the request
	if(canAnswerCSR(currentChannelSenseRequest))
	{
		answerCSR(currentChannelSenseRequest);
		return notAgain;
	}

	return ( now + request->getSenseTimeout() );
}

void BaseDecider::handleSenseRequestTimeout(CSRInfo& requestInfo) {
	answerCSR(requestInfo);
}

int BaseDecider::getSignalState(AirFrame* frame) {
	if(frame == currentSignal.first)
		return currentSignal.second;

	return NEW;
}

void BaseDecider::setChannelIdleStatus(bool isIdle) {
	isChannelIdle = isIdle;

	//check if there is an ChannelSenseRequest we can answer now
	if(canAnswerCSR(currentChannelSenseRequest)){
		phy->cancelScheduledMessage(currentChannelSenseRequest.first);
		answerCSR(currentChannelSenseRequest);
	}
}

bool BaseDecider::canAnswerCSR(const CSRInfo& requestInfo)
{
	if(requestInfo.first == 0)
		return false;

	bool modeFulfilled = false;

	switch(requestInfo.first->getSenseMode())
	{
	case UNTIL_IDLE:
		modeFulfilled = isChannelIdle;
		break;
	case UNTIL_BUSY:
		modeFulfilled = !isChannelIdle;
		break;
	}

	return modeFulfilled 									//CSR mode is fulfilled
		   || (phy->getSimTime() ==   requestInfo.second 	//or timeout is reached
								    + requestInfo.first->getSenseTimeout());
}

double BaseDecider::calcChannelSenseRSSI(simtime_t start, simtime_t end) {
	Mapping* rssiMap = calculateRSSIMapping(start, end);

	// the sensed RSSI-value is the maximum value between (and including) the interval-borders
	double rssi = MappingUtils::findMax(*rssiMap, Argument(start), Argument(end));

	delete rssiMap;

	return rssi;
}

void BaseDecider::answerCSR(CSRInfo& requestInfo) {

	double rssiValue = calcChannelSenseRSSI(requestInfo.second, phy->getSimTime());

	//"findMax()" returns "-DBL_MAX" on empty mappings
	if (rssiValue < 0)
		rssiValue = 0;

	// put the sensing-result to the request and
	// send it to the Mac-Layer as Control-message (via Interface)
	requestInfo.first->setResult( ChannelState(isChannelIdle, rssiValue) );
	phy->sendControlMsg(requestInfo.first);

	requestInfo.first = 0;
}

Mapping* BaseDecider::calculateSnrMapping(AirFrame* frame)
{
	/* calculate Noise-Strength-Mapping */
	Signal& signal = frame->getSignal();

	simtime_t start = signal.getSignalStart();
	simtime_t end = start + signal.getSignalLength();

	Mapping* noiseMap = calculateRSSIMapping(start, end, frame);
	assert(noiseMap);
	ConstMapping* recvPowerMap = signal.getReceivingPower();
	assert(recvPowerMap);

	//TODO: handle noise of zero (must not devide with zero!)
	Mapping* snrMap = MappingUtils::divide( *recvPowerMap, *noiseMap, 0.0 );

	delete noiseMap;
	noiseMap = 0;

	return snrMap;
}

/*
Mapping* BaseDecider::calculateRSSIMapping(	simtime_t start,
										simtime_t end,
										AirFrame* exclude)
{
	if(exclude)
		debugEV << "Creating RSSI map excluding AirFrame with id " << exclude->getId() << endl;
	else
		debugEV << "Creating RSSI map." << endl;

	AirFrameVector airFrames;

	// collect all AirFrames that intersect with [start, end]
	phy->getChannelInfo(start, end, airFrames);

	//TODO: create a "MappingUtils:createMappingFrom()"-method and use it here instead
	//of abusing the add method
	// create an empty mapping
	Mapping* resultMap = MappingUtils::createMapping(0.0, DimensionSet::timeDomain);

	//add thermal noise
	ConstMapping* thermalNoise = phy->getThermalNoise(start, end);
	if(thermalNoise) {
		Mapping* tmp = resultMap;
		resultMap = MappingUtils::add(*resultMap, *thermalNoise);
		delete tmp;
	}

	// otherwise, iterate over all AirFrames (except exclude)
	// and sum up their receiving-power-mappings
	AirFrameVector::iterator it;
	for (it = airFrames.begin(); it != airFrames.end(); it++)
	{
		// the vector should not contain pointers to 0
		assert (*it != 0);

		// if iterator points to exclude (that includes the default-case 'exclude == 0')
		// then skip this AirFrame
		if ( *it == exclude ) continue;

		// otherwise get the Signal and its receiving-power-mapping
		Signal& signal = (*it)->getSignal();

		// backup pointer to result map
		// Mapping* resultMapOld = resultMap;

		// TODO1.1: for testing purposes, for now we don't specify an interval
		// and add the Signal's receiving-power-mapping to resultMap in [start, end],
		// the operation Mapping::add returns a pointer to a new Mapping

		ConstMapping* recvPowerMap = signal.getReceivingPower();
		assert(recvPowerMap);

		// Mapping* resultMapNew = Mapping::add( *(signal.getReceivingPower()), *resultMap, start, end );

		debugEV << "Adding mapping of Airframe with ID " << (*it)->getId()
				<< ". Starts at " << signal.getSignalStart()
				<< " and ends at " << signal.getSignalStart() + signal.getSignalLength() << endl;

		Mapping* resultMapNew = MappingUtils::add( *recvPowerMap, *resultMap, 0.0 );

		// discard old mapping
		delete resultMap;
		resultMap = resultMapNew;
		resultMapNew = 0;
	}

	return resultMap;
}
*/

Mapping* BaseDecider::calculateRSSIMapping(    simtime_t start, simtime_t end, AirFrame* exclude)
{
    if(exclude)
        debugEV << "Creating RSSI map excluding AirFrame with id " << exclude->getId() << endl;
    else
    debugEV << "Creating RSSI map." << endl;

    AirFrameVector airFrames;

    // collect all AirFrames that intersect with [start, end]
    phy->getChannelInfo(start, end, airFrames);

    //TODO: create a "MappingUtils:createMappingFrom()"-method and use it here instead
    //of abusing the add method
    // create an empty mapping
    Mapping* resultMap = MappingUtils::createMapping(0.0, DimensionSet::timeDomain);

    //add thermal noise
    ConstMapping* thermalNoise = phy->getThermalNoise(start, end);

    if(thermalNoise) {
        Mapping* tmp = resultMap;
        resultMap = MappingUtils::add(*resultMap, *thermalNoise);

        delete tmp;
    }

    // otherwise, iterate over all AirFrames (except exclude)
    // and sum up their receiving-power-mappings
    AirFrameVector::iterator it;
    for (it = airFrames.begin(); it != airFrames.end(); it++)
    {
        // the vector should not contain pointers to 0
        assert (*it != 0);

        // if iterator points to exclude (that includes the default-case 'exclude == 0')
        // then skip this AirFrame
        if ( *it == exclude ) continue;

        // otherwise get the Signal and its receiving-power-mapping
        Signal& signal = (*it)->getSignal();

        // backup pointer to result map
        // Mapping* resultMapOld = resultMap;

        // TODO1.1: for testing purposes, for now we don't specify an interval
        // and add the Signal's receiving-power-mapping to resultMap in [start, end],
        // the operation Mapping::add returns a pointer to a new Mapping

        ConstMapping* recvPowerMap = signal.getReceivingPower();
        assert(recvPowerMap);


        //*****************/
        //for debugging
        //recvPowerMap = Daten des Störers
        simtime_t start_interferer = signal.getSignalStart();
        simtime_t after_end_interferer = signal.getSignalStart() + signal.getSignalLength() + signal.getSignalLength() + signal.getSignalLength();
        simtime_t end_interferer = signal.getSignalStart() + signal.getSignalLength();

        double power_interferer_begin_nutzpaket = recvPowerMap->getValue(start);
        double power_begin_interferer = recvPowerMap->getValue(start_interferer);
        double power_interferer_end_nutzpaket = recvPowerMap->getValue(end);
        double power_end_interferer = recvPowerMap->getValue(end_interferer );
        double power_after_end_interferer = recvPowerMap->getValue(after_end_interferer );

        double noise_power_begin_nutzpaket = resultMap->getValue(start);
        double noise_power_begin_interference_paket = resultMap->getValue(start_interferer);
        double noise_power_end_nutzpaket = resultMap->getValue(end);
        //****************/


        // Mapping* resultMapNew = Mapping::add( *(signal.getReceivingPower()), *resultMap, start, end );

        debugEV << "Adding mapping of Airframe with ID " << (*it)->getId()
                << ". Starts at " << signal.getSignalStart()
                << " and ends at " << signal.getSignalStart() + signal.getSignalLength() << endl;

        Mapping* resultMapNew = MappingUtils::add( *recvPowerMap, *resultMap, 0.0 );

        //*****************/
        //for debugging
        simtime_t before_use_package = start - signal.getSignalLength();
        double interference_power_vor_nutzpaket = resultMapNew->getValue(start - signal.getSignalLength());
        double interference_power_begin_nutzpaket = resultMapNew->getValue(start);
        double interference_power_begin_interferer = resultMapNew->getValue(start_interferer);
        double interference_power_end_nutzpaket = resultMapNew->getValue(end);
        double interference_power_end_interferer = resultMapNew->getValue(end_interferer);
        double interference_power_after_interferer = resultMapNew->getValue(after_end_interferer);
        //*****************/


        // discard old mapping
        delete resultMap;
        resultMap = resultMapNew;
        resultMapNew = 0;
    }
    return resultMap;
}
