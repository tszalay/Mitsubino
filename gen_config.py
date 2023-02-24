#!/usr/bin/env python

import argparse
import yaml

units = {
    "hp_basement":"Basement",
    "hp_office":"Office",
    "hp_bedroom":"Bedroom",
    "hp_kitchen":"Kitchen",
    "hp_livingroom":"Living Room"
}

parser = argparse.ArgumentParser()
parser.add_argument("template", help="YAML template to use. Allowed keys are NAME, HOSTNAME, HANAME")
args = parser.parse_args()

config_base = open(args.template).read().strip()

config = None
for host, name in units.items():
    haname = name.replace(" ","_").lower()
    this_yaml = yaml.safe_load(config_base.replace("HANAME",haname).replace("HOSTNAME",host).replace("NAME",name))
    if config is None:
        config = this_yaml
    elif isinstance(config, list):
        config.extend(this_yaml)
    elif isinstance(config, dict):
        for k,v in this_yaml.items():
            config[k].extend(v)

print(yaml.dump(config))
