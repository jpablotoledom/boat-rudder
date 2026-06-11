#!/bin/bash
mongorestore --drop --db boat_rudder ./db_backup/boat_rudder
# mongorestore --db boat_rudder ./db_backup/boat_rudder