FROM openjdk:8-alpine

RUN apk update && apk add -u gcc musl-dev

ENV java_home /usr/lib/jvm/default-jvm/jre

RUN cp $java_home/bin/java $java_home/bin/origjava

RUN mkdir -p /opt/src

COPY src/java.c /opt/src/

RUN gcc -o /opt/java /opt/src/java.c

RUN mv /opt/java $java_home/bin/java && md5sum $java_home/bin/java

RUN apk del gcc musl-dev
