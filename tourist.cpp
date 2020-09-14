#include <mpi.h>
#include <chrono>
#include <unistd.h>
#include <thread>
#include <cstdio>
#include <ctime>
#include <csignal>
#include <iostream>
#include <unistd.h>		
#include "tourist.h"

	// Sent once by main thread
#define LAUNCH 0
	// Costume request message
#define COSTUME_REQ 1 // SREQ
	// Boat request message
#define BOAT_REQ 2 // BREQ
	// Costume accept message
#define COSTUME_ACK 3
	// Boat accept message
#define BOAT_ACK 4
	// Sent by process when can't fit in boad
	// Boat sent to Cruise
#define CRUISE 5
	// Sent by process when boat arrives in harbor
	// Releases all the resources
#define CRUISE_END 6

#define INIT 100
#define PENDING 101
#define SUIT_CRITICAL 102
#define BOAT_CRITICAL 103
#define ON_BOAT 104


Tourist::Tourist(int costumes, int boats, int tourists, int max_capacity) {
	
	size = MPI::COMM_WORLD.Get_size();
	rank = MPI::COMM_WORLD.Get_rank();
	
	srand(time(NULL) * rank);
	this->state = INIT;
	for (int i = 0; i < tourists; i++) {
		this->process_list.push_back(i);
	}
	this->costumes = costumes;
	this->have_costume = 0;
	
	this->clock = 0;
	this->event_mutex.lock();
	this->running = true;
	this->request_id = 0;
	
	// proces madka
	//if (rank == 0) {
		
		for (int i = 0; i < boats; i++) {
			s_boat boat;
			boat.id = i;
			boat.capacity = 10 + 2 * i; //rand()%10+10; // 10-20
			boat.state = 0;
			boats_list.push_back(boat);
		}
		
		/*for (int i = 1; i < size; i++)
			MPI_Send(&boats_list, boats, MPI_UNSIGNED_LONG, i, LAUNCH, MPI_COMM_WORLD);
	} else {
		MPI_Recv(&boats_list, boats, MPI_UNSIGNED_LONG, MPI_ANY_SOURCE, LAUNCH, MPI_COMM_WORLD, 0);
		for (auto x: boats_list) {
			printf("[rank: %d, %d %d]", rank, x.id, x.capacity);
		}
	}*/
	this->state = PENDING;
	printf("[Rank: %d|Clock: %d]: %s\n", rank, clock, "Created a tourist!");
}

void Tourist::createMonitorThread() {
	new std::thread(&Tourist::monitorThread, this);
}

/** >>Done<<
 * Function creates and returns a new request with one value.
 */
s_request Tourist::create_request(int value) {
	s_request request;
	request.value = value;
	request.sender_id = rank;
	
	clock_mutex.lock();
	
	request.id = request_id++;
	request.clock = clock++;
	
	clock_mutex.unlock();
	
	return request;
}

/** >>Done<<
 * Function creates and returns a new request with two values.
 */
s_request Tourist::create_request(int value, int value2) {
	s_request request = create_request(value);
	request.value2 = value2;
	
	return request;
}


bool Tourist::handleResponse(s_request *result, int status) {
	bool resolved = true;
	switch(state) {
		case PENDING:
		{
			switch(status) {
				case COSTUME_REQ:
				{
					s_request ack = create_request(0);
					log("Sending Costume ACK to " + std::to_string(result->sender_id));
					MPI_Send(&ack, sizeof(s_request), MPI_BYTE, result->sender_id, COSTUME_ACK, MPI_COMM_WORLD);
					break;
				}
				
				case BOAT_REQ: // (-1, -1)
				{
					s_request ack = create_request(-1, -1);
					log("Sending Boat ACK to " + std::to_string(result->sender_id));
					MPI_Send(&ack, sizeof(s_request), MPI_BYTE, result->sender_id, BOAT_ACK, MPI_COMM_WORLD);
					break;
				}
				
				default:
				{
					resolved = false;
					break;
				}
			}
			break;
		}
		
		case SUIT_CRITICAL:
		{
			switch(status) {
				case COSTUME_ACK:
				{
					ack_mutex.lock();
					if (result->value == 0)
						ack++;
					log("Acquired Costume ACK from " + std::to_string(result->sender_id));
					if (ack >= process_list.size()-costumes) {
						setState(BOAT_CRITICAL);
						event_mutex.unlock();
					}
					ack_mutex.unlock();
					break;
				}
				
				case COSTUME_REQ:
				{
					if (result->clock < clock || (result->clock == clock && rank > result->id)) { // send ok
						s_request ack = create_request(0);
						log("Sending Costume ACK to " + std::to_string(result->sender_id));
						MPI_Send(&ack, sizeof(s_request), MPI_BYTE, result->sender_id, COSTUME_ACK, MPI_COMM_WORLD);
					}
					else { // cache
						addToLamportVector(result);
					}
					break;
				}
				
				case BOAT_REQ:
				{
					s_request ack = create_request(-1, -1);
					printf("===SUIT>>>[%d|%d] BOAT_ACK to [%d|%d]\n", rank, clock, result->sender_id, result->clock);
					MPI_Send(&ack, sizeof(s_request), MPI_BYTE, result->sender_id, BOAT_ACK, MPI_COMM_WORLD);
					break;
				}
				
				default:
				{
					resolved = false;
					break;
				}
			}
			break;
		}
		
		case BOAT_CRITICAL:
		{
			switch(status) {
				case COSTUME_REQ:
				{
					log("Costume request from " + std::to_string(result->sender_id) + " suspended");
					addToLamportVector(result);
					break;
				}
				
				case BOAT_REQ:
				{
					if (result->clock > clock) { // dodaj do tablicy
						log("Boat request from " + std::to_string(result->sender_id) + " suspended");
						addToLamportVector(result);
					}
					else {
						s_request ack = create_request(-1, -1);
						log("Sending Boat ACK to " + std::to_string(result->sender_id));
						MPI_Send(&ack, sizeof(s_request), MPI_BYTE, result->sender_id, BOAT_ACK, MPI_COMM_WORLD);
					}
					break;
				}
				
				case BOAT_ACK:
				{
					log("Received Boat ACK from " + std::to_string(result->sender_id));
					for (auto x : boats_list) {
						if (x.id == result->value) {
							x.occupied += result->value2;
						}
					}
					ack_mutex.lock();
					ack++;
					if (ack == process_list.size()) {
						event_mutex.unlock();
					}
					ack_mutex.unlock();
					break;
				}
				
				case CRUISE_END:
				{
					for (auto boat : boats_list) {
						boat.occupied = 0;
					}
					ack = 0;
					s_request boat_request = create_request(capacity);
					boat_request.clock = last_request_clock;
					broadcastRequest(&boat_request, BOAT_REQ);
					break;
				}
				
				default:
				{
					resolved = false;
					break;
				}
			}
			break;
		}
		
		case ON_BOAT:
		{
			switch(status) {
				case COSTUME_REQ:
				{
					addToLamportVector(result);
					break;
				}
				
				case BOAT_REQ:
				{
					s_request ack = create_request(boat_id, capacity); // (boat id, size)
					MPI_Send(&ack, sizeof(s_request), MPI_BYTE, result->sender_id, BOAT_ACK, MPI_COMM_WORLD);
					break;
				}

				case CRUISE:
				{
					is_captain = false;
					if(result->value == boat_id && result->value2 == rank){
						is_captain = true;
					}
					event_mutex.unlock();
					break;
				}
				
				case CRUISE_END:
				{
					if(result->value == boat_id){
						event_mutex.unlock();
					}
					break;
				}
				
				default:
				{
					resolved = false;
					break;
				}
			}
			break;
		}
		
		default:
		{
			resolved = false;
			break;
		}
	}
	return resolved;
}


/** 
 * Function to manage received MPI messages.
 */
void Tourist::monitorThread() {
	while(running) {
		s_request result;
		MPI_Status status;
		MPI_Recv(&result, sizeof(s_request), MPI_BYTE, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
		result.type = status.MPI_TAG;
		
		handleResponse(&result, result.type);

		clock_mutex.lock();
		result.id = request_id;
		request_id++;
		clock = std::max(clock, result.clock) + 1;
		clock_mutex.unlock();
	}
}

void Tourist::setState(int value) {
	if (!lamport_vector.empty()) {
			std::vector<s_request>::iterator it;
			for (it = lamport_vector.begin(); it < lamport_vector.end(); it++) {
				bool x = handleResponse(&(*it), it->type); 
				// TODO: change status (status not present in scope)
				if (x) {
					removeFromLamportVector(it->id);
				}
			}
		}
		
	state = value;
}

void Tourist::runPerformThread() {
	while(running) {
		// perform all tasks
		// 1. initialization
		
		// 2. wait to spawn
		
		std::this_thread::sleep_for(std::chrono::milliseconds((rand()%10000) + 2000));
		setState(SUIT_CRITICAL);
		
		// 3. sending costume request
		
		log("Requesting costume...");
		ack = 0;
		s_request costume_request = create_request(0);
		broadcastRequest(&costume_request, COSTUME_REQ);
		
		// 4. receive costume ACK for every proc -> get a costume
		
		event_mutex.lock();
		log("Received a costume!");
		setState(BOAT_CRITICAL);
		have_costume = 1;
		
		// 5. sending boat request
		
		capacity = (rand()%5) + 2; // place needed on boat
		
		log("Requesting a boat");
		ack = 0;
		s_request boat_request = create_request(capacity);
		last_request_clock = boat_request.clock;
		broadcastRequest(&boat_request, BOAT_REQ);
		
		// 6. receive boat ACK -> get a boat
		for (;;) {
			event_mutex.lock();
			bool found = false;
			for (auto boat : boats_list) {
				if (boat.state == 0 && boat.capacity - boat.occupied <= capacity) {
					boat_id = boat.id;
					found = true;
					break;
				}
				else if (boat.state == 0) {
					if(boat.tourists_list.size() > 0){
						s_request cruise_request = create_request(boat.id, boat.tourists_list[0]);
						broadcastRequest(&cruise_request, CRUISE);
					}
				}
			}
			if (found) {
				break;
			}
		}
		log("Boat received!");
		setState(ON_BOAT);

		// 7. wait for cruise launch
		event_mutex.lock();

		// 8. end the cruise
		if (is_captain) {
			int sleep_time = rand() % 3 + 3;
			sleep(sleep_time);
			s_request cruise_end_req = create_request(boat_id);
			broadcastRequest(&cruise_end_req, CRUISE_END);
		}
		else {
			event_mutex.lock();
		}

		// 9. release the boat
		boat_id = -1;

		// 10. release costume and start over
		have_costume = 0;
	}
}

/** >>Done<<
 * Function broadcasting request to every proccess.
 */
void Tourist::broadcastRequest(s_request *request, int request_type) {
	for (int i = 0; i < size; i++)
		if (i != rank) {
			MPI_Send(request, sizeof(s_request), MPI_BYTE, i, request_type, MPI_COMM_WORLD);
			//printf("Broadcast %d to %d\n", rank, i);
		}
}

/** >>Done<< (in theory should be alright)
 * Adding cached request to local list of events.
 */
void Tourist::addToLamportVector(s_request *request) {
	lamport_mutex.lock();
	std::vector<s_request>::iterator it;
	
	for (it = lamport_vector.begin(); it < lamport_vector.end(); it++) {
		if (it->clock > request->clock) {
			break;
		}
		// setting priority for the same times
		if (it->clock == request->clock && it->sender_id > request->sender_id) {
			break;
		}
	}
	lamport_vector.insert(it, *request);
	lamport_mutex.unlock();
}

/** >>Done<< (as above)
 * Removing first spotted request in local list of events.
 */
void Tourist::removeFromLamportVector(int id) {
	lamport_mutex.lock();
	std::vector<s_request>::iterator it;
	
	for (it = lamport_vector.begin(); it < lamport_vector.end(); it++) {
		if (it->id == id) {
			lamport_vector.erase(it);
			break;
		}
	}
	lamport_mutex.unlock();
}

void Tourist::log(std::string message){
	printf("[ID: %d|Clock: %d]: %s\n", rank, clock, message.c_str());
}