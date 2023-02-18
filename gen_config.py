#!/usr/bin/env python

import yaml
from collections import defaultdict

units = {
    "hp_basement":"Basement",
    "hp_office":"Office",
    "hp_bedroom":"Bedroom",
    "hp_kitchen":"Kitchen",
    "hp_livingroom":"Living Room"
}

config_base = open("config_template.yaml").read().strip()

config = {"mqtt":defaultdict(list)}
for host, name in units.items():
    this_yaml = yaml.safe_load(config_base.replace("HOSTNAME",host).replace("NAME",name))
    for k,v in this_yaml["mqtt"].items():
        config["mqtt"][k].extend(v)

config["mqtt"] = dict(**config["mqtt"])
yaml.dump(config, open("config.yaml","w"))
