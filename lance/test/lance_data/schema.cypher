INSTALL LANCE;
LOAD LANCE;
CREATE NODE TABLE city(id INT32, name STRING, population INT64, PRIMARY KEY(id)) WITH (format = 'lance');
CREATE NODE TABLE user(id INT32, name STRING, age INT64, PRIMARY KEY(id)) WITH (format = 'lance');
CREATE REL TABLE follows(FROM user TO user, since INT32) WITH (format = 'lance');
CREATE REL TABLE livesin(FROM user TO city) WITH (format = 'lance');
