# docker build -t clickhouse/postgresql-dotnet-client .
# PostgreSQL C# client docker container

FROM ubuntu:22.04

WORKDIR /testapp

RUN apt-get update \
    && apt-get install -y software-properties-common build-essential dotnet-sdk-8.0 curl

COPY testapp.csproj testapp.csproj
RUN dotnet restore

COPY Program.cs Program.cs
