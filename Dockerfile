FROM openjdk:8-alpine

RUN apk update && apk add -u gcc musl-dev

ENV jre_home /usr/lib/jvm/default-jvm/jre

RUN for jh in $JAVA_HOME $jre_home; do cp $jh/bin/java $jh/bin/origjava; done

RUN mkdir -p /opt/src

COPY src/java.c /opt/src/

RUN gcc -o /opt/java /opt/src/java.c

RUN for jh in $JAVA_HOME $jre_home; \
      do cp /opt/java $jh/bin/java && md5sum $jh/bin/java $jh/bin/origjava; \
    done && rm /opt/java

RUN apk del gcc musl-dev
